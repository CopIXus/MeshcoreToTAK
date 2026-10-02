#include "MyMesh.h"
#include <SPIFFS.h>
#include <helpers/AdvertDataHelpers.h>

void MyMesh::beginFs() {
  BaseChatMesh::begin();

  IdentityStore store(SPIFFS, "/identity");
  char name[32] = "TAK-GW";
  if (!store.load("_main", self_id, name, sizeof(name))) {
    ((StdRNG*)getRNG())->begin(radio_driver.getRngSeed());
    self_id = mesh::LocalIdentity(getRNG());
    int count = 0;
    while (count < 10 && (self_id.pub_key[0] == 0x00 || self_id.pub_key[0] == 0xFF)) {
      self_id = mesh::LocalIdentity(getRNG());
      count++;
    }
    store.save("_main", self_id);
    Serial.println("[MESH] Created new identity");
  } else {
    Serial.print("[MESH] Identity ");
    mesh::Utils::printHex(Serial, self_id.pub_key, PUB_KEY_SIZE);
    Serial.println();
  }

  applyRadioFromConfig();
  applyChannelsFromConfig();
  if (_cfg && _cfg->prefs.advert_on) _advert_due = true;  // goes out once the clock is set
}

bool MyMesh::sendSelfAdvert() {
  const TakPrefs& p = _cfg->prefs;
  const char* name = p.node_name[0] ? p.node_name : "TAK-GW";
  uint8_t app_data[MAX_ADVERT_DATA_SIZE];
  bool has_loc = p.chat_lat != 0.0f || p.chat_lon != 0.0f;
  AdvertDataBuilder b = has_loc ? AdvertDataBuilder(ADV_TYPE_CHAT, name, p.chat_lat, p.chat_lon)
                                : AdvertDataBuilder(ADV_TYPE_CHAT, name);
  uint8_t len = b.encodeTo(app_data);
  mesh::Packet* pkt = createAdvert(self_id, app_data, len);
  if (!pkt) return false;
  sendFlood(pkt);
  Serial.printf("[MESH] advert sent: %s %s\n", name, has_loc ? "with location" : "(no location)");
  return true;
}

void MyMesh::applyChannelsFromConfig() {
  if (!_cfg) return;
  for (int i = 0; i < TAK_MAX_CHAT; i++) {
    const TakChatChannel& c = _cfg->prefs.chat[i];
    ChannelDetails d;
    memset(&d, 0, sizeof(d));
    if (c.enabled && (c.secret_len == 16 || c.secret_len == 32)) {
      memcpy(d.channel.secret, c.secret, c.secret_len);
      StrHelper::strncpy(d.name, c.name, sizeof(d.name));
      setChannel(i, d);
      ChannelDetails set;
      getChannel(i, set);
      Serial.printf("[CHAT] ch%d '%s' (ch#%02x) <-> TAK room '%s'\n", i, c.name, set.channel.hash[0], c.room);
    } else {
      setChannel(i, d);  // all-zero key: its MAC never validates, so the slot hears nothing
    }
  }

  // MeshCore's well-known Public channel key
  static const uint8_t PUBLIC_KEY[16] = {0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a,
                                         0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72};
  ChannelDetails pub;
  memset(&pub, 0, sizeof(pub));
  if (_cfg->prefs.public_on) {
    memcpy(pub.channel.secret, PUBLIC_KEY, sizeof(PUBLIC_KEY));
    StrHelper::strncpy(pub.name, "Public", sizeof(pub.name));
    Serial.printf("[CHAT] Public -> TAK room '%s' (listen only)\n", _cfg->prefs.public_room);
  }
  setChannel(TAK_PUBLIC_SLOT, pub);
}

void MyMesh::onChannelMessageRecv(const mesh::GroupChannel& channel, mesh::Packet* pkt, uint32_t timestamp,
                                  const char* text) {
  (void)pkt;
  (void)timestamp;
  int ch = findChannelIdx(channel);
  if (!_cfg || !_client || ch < 0) return;
  bool on = ch == TAK_PUBLIC_SLOT ? _cfg->prefs.public_on : (ch < TAK_MAX_CHAT && _cfg->prefs.chat[ch].enabled);
  if (!on) return;

  // MeshCore group text is "<sender>: <message>"
  char sender[TAK_CALLSIGN_LEN] = "MeshCore";
  const char* msg = text;
  const char* sep = strstr(text, ": ");
  if (sep && sep - text < (int)sizeof(sender)) {
    memcpy(sender, text, sep - text);
    sender[sep - text] = 0;
    msg = sep + 2;
  }
  Serial.printf("[CHAT] mesh ch%d %s -> TAK room %s: %s\n", ch, sender, _client->roomFor(ch), msg);
  _client->noteChat(true, ch, sender, msg);
  if (_cfg->prefs.enabled && !_client->queueChat(ch, sender, msg)) {
    Serial.println("[CHAT] not sent to TAK (no link / no time yet)");
  }
}

void MyMesh::applyRadioFromConfig() {
  if (!_cfg) return;
  float freq = _cfg->prefs.lora_freq > 1 ? _cfg->prefs.lora_freq : (float)LORA_FREQ;
  float bw = _cfg->prefs.lora_bw > 1 ? _cfg->prefs.lora_bw : (float)LORA_BW;
  uint8_t sf = _cfg->prefs.lora_sf ? _cfg->prefs.lora_sf : (uint8_t)LORA_SF;
  uint8_t cr = _cfg->prefs.lora_cr ? _cfg->prefs.lora_cr : (uint8_t)LORA_CR;
  radio_driver.setParams(freq, bw, sf, cr);
  Serial.printf("[MESH] Radio %.3f MHz BW=%.1f SF=%u CR=%u\n", freq, bw, sf, cr);
}

void MyMesh::onDiscoveredContact(ContactInfo& contact, bool is_new, uint8_t path_len, const uint8_t* path) {
  (void)is_new;
  (void)path_len;
  (void)path;

  bool gps = TakNodes::hasValidGps(contact);
  Serial.printf("[MESH] ADVERT %s lat=%ld lon=%ld%s\n", contact.name, (long)contact.gps_lat,
                (long)contact.gps_lon, gps ? "" : " (no position - not sent to TAK)");

  if (!_nodes || !_cfg) return;

  _nodes->rx.adverts++;
  if (gps) _nodes->rx.adverts_gps++;
  strncpy(_nodes->rx.last_advert_name, contact.name, sizeof(_nodes->rx.last_advert_name) - 1);
  _nodes->rx.last_advert_name[sizeof(_nodes->rx.last_advert_name) - 1] = 0;
  _nodes->rx.last_advert_gps = gps;

  // GPS is checked first (upsert rejects adverts without a position), then the name rules.
  TakNodeRecord* rec = _nodes->upsertFromContact(contact, self_id.pub_key);
  if (!rec) return;

  if (!TakNodes::passesFilter(rec->name, _cfg->prefs)) {
    Serial.printf("[TAK] skip (name filter): %s\n", rec->name);
    return;
  }
  if (_client && _cfg->prefs.enabled) {
    if (_client->queuePoint(*rec)) {
      _nodes->markSent(rec, millis());
      Serial.printf("[TAK] queued %s %s\n", rec->uid, rec->name);
    }
  }
}

void MyMesh::logRx(mesh::Packet* packet, int len, float score) {
  (void)len;
  (void)score;
  if (!_nodes) return;
  _nodes->rx.packets++;
  _nodes->rx.last_rx_ms = millis();
  _nodes->rx.last_rssi = radio_driver.getLastRSSI();
  _nodes->rx.last_snr = radio_driver.getLastSNR();
  if (packet->getPayloadType() == PAYLOAD_TYPE_GRP_TXT && packet->payload_len) {
    Serial.printf("[MESH] RX group text ch#%02x rssi=%.0f snr=%.1f\n", packet->payload[0], _nodes->rx.last_rssi,
                  _nodes->rx.last_snr);
    return;
  }
  Serial.printf("[MESH] RX type=%u rssi=%.0f snr=%.1f\n", (unsigned)packet->getPayloadType(),
                _nodes->rx.last_rssi, _nodes->rx.last_snr);
}

void MyMesh::loopGateway() {
  loop();

  if (_cfg && _cfg->prefs.advert_on && _cfg->prefs.advert_hours && _last_advert_ms &&
      millis() - _last_advert_ms >= _cfg->prefs.advert_hours * 3600000UL) {
    _advert_due = true;
  }
  // Adverts carry a timestamp; nodes discard ones older than they have seen, so wait for NTP.
  if (_advert_due && _client && _client->nowUtc() && sendSelfAdvert()) {
    _advert_due = false;
    _last_advert_ms = millis();
  }

  TakChatIn m;
  if (_client && _client->popChatIn(m)) {
    ChannelDetails d;
    if (!getChannel(m.ch, d)) return;
    uint32_t ts = getRTCClock()->getCurrentTime();
    if (sendGroupMessage(ts, d.channel, m.sender, m.text, strlen(m.text))) {
      _client->noteChat(false, m.ch, m.sender, m.text);
    } else {
      Serial.println("[CHAT] mesh send failed (packet pool full)");
    }
  }
}
