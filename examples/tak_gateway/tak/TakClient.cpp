#include "TakClient.h"
#include "TakCot.h"
#include "TakCerts.h"
#include "TakText.h"
#include <WiFi.h>
#include <time.h>
#include <string.h>

#include "esp_tls.h"
#include "esp_err.h"
#include "mbedtls/error.h"
#include "lwip/sockets.h"

#define TLS ((esp_tls_t*)_tls)

TakClient::TakClient() {
  _last_error[0] = 0;
}

TakClient::~TakClient() {
  disconnectTls();
}

void TakClient::begin(TakConfig* cfg, TakNodes* nodes) {
  _cfg = cfg;
  _nodes = nodes;
  _last_error[0] = 0;
  _q_head = _q_tail = _q_count = 0;
  snprintf(_gw_uid, sizeof(_gw_uid), "meshcore-gw-%012llx", (unsigned long long)(ESP.getEfuseMac() & 0xFFFFFFFFFFFFULL));
  if (_cfg && _cfg->prefs.enabled) setState(TakLinkState::WaitWifi);
  else setState(TakLinkState::Disabled);
}

void TakClient::setError(const char* msg) {
  strncpy(_last_error, msg ? msg : "", sizeof(_last_error) - 1);
  _last_error[sizeof(_last_error) - 1] = 0;
}

void TakClient::setState(TakLinkState s) { _state = s; }

const char* TakClient::stateName() const {
  switch (_state) {
    case TakLinkState::Disabled: return "DISABLED";
    case TakLinkState::WaitWifi: return "WAIT_WIFI";
    case TakLinkState::WaitNtp: return "WAIT_NTP";
    case TakLinkState::Connecting: return "CONNECTING";
    case TakLinkState::Connected: return "CONNECTED";
    case TakLinkState::Backoff: return "BACKOFF";
    case TakLinkState::Error: return "ERROR";
  }
  return "?";
}

void TakClient::requestConnect() {
  if (!_cfg || !_cfg->prefs.enabled) {
    setState(TakLinkState::Disabled);
    return;
  }
  _backoff_ms = 1000;
  setState(TakLinkState::WaitWifi);
}

time_t TakClient::nowUtc() const {
  time_t now = time(nullptr);
  // ESP32Board seeds the clock with 15 May 2024 at boot; only trust time after a real NTP sync.
  if (now < 1767225600) return 0;  // 2026-01-01
  return now;
}

void TakClient::disconnectTls() {
  if (_tls) {
    esp_tls_conn_destroy(TLS);
    _tls = nullptr;
  }
}

void TakClient::captureTlsError(const char* prefix) {
  char msg[96] = {0};
  if (_tls) {
    int esp_code = 0;
    int mbedtls_code = 0;
    esp_err_t err = esp_tls_get_and_clear_last_error(TLS->error_handle, &esp_code, &mbedtls_code);
    char mbed_str[48] = {0};
    if (mbedtls_code) mbedtls_strerror(mbedtls_code, mbed_str, sizeof(mbed_str));
    snprintf(msg, sizeof(msg), "%s esp=%s/%d mbed=%s", prefix ? prefix : "TLS",
             esp_err_to_name(err), esp_code, mbed_str[0] ? mbed_str : "none");
  } else {
    snprintf(msg, sizeof(msg), "%s (no tls handle)", prefix ? prefix : "TLS");
  }
  setError(msg);
  Serial.printf("[TAK] %s\n", msg);
}

bool TakClient::loadCertPems() {
  if (!_cfg || !_cfg->hasClientCerts()) {
    setError("missing cert files — upload Portal .pem + .key");
    return false;
  }
  _ca_pem = _cfg->readFile(_cfg->caPath());
  _cert_pem = _cfg->readFile(_cfg->certPath());
  _key_pem = _cfg->readFile(_cfg->keyPath());
  if (_ca_pem.isEmpty() || _cert_pem.isEmpty() || _key_pem.isEmpty()) {
    setError("empty cert files");
    return false;
  }

  String kerr;
  if (!TakCerts::preparePrivateKey(_key_pem, _cfg->prefs.key_passphrase, kerr)) {
    setError(kerr.c_str());
    return false;
  }
  if (TakCerts::keyLooksEncrypted(_cfg->readFile(_cfg->keyPath()))) {
    _cfg->writeFile(_cfg->keyPath(), _key_pem);
  }
  if (_ca_pem.indexOf("BEGIN CERTIFICATE") < 0) {
    setError("CA missing — re-upload Portal .pem (includes chain)");
    return false;
  }
  Serial.printf("[TAK] certs loaded ca=%u cert=%u key=%u heap=%u time=%ld\n", (unsigned)_ca_pem.length(),
                (unsigned)_cert_pem.length(), (unsigned)_key_pem.length(), (unsigned)ESP.getFreeHeap(),
                (long)time(nullptr));
  return true;
}

bool TakClient::connectTls() {
  if (!_cfg->hasTakHost()) {
    setError("no TAK host");
    return false;
  }
  if (!loadCertPems()) return false;

  setState(TakLinkState::Connecting);
  disconnectTls();
  WiFi.mode(WIFI_STA);

  int port = _cfg->prefs.tak_port ? _cfg->prefs.tak_port : 8089;
  Serial.printf("[TAK] Connecting %s:%d heap=%u\n", _cfg->prefs.tak_host, port, (unsigned)ESP.getFreeHeap());

  IPAddress ip;
  if (WiFi.hostByName(_cfg->prefs.tak_host, ip)) {
    Serial.printf("[TAK] DNS %s -> %s\n", _cfg->prefs.tak_host, ip.toString().c_str());
  }

  _tls = esp_tls_init();
  if (!_tls) {
    setError("esp_tls_init failed");
    return false;
  }

  esp_tls_cfg_t cfg = {};
  // PEM buffers must include the trailing NUL in the reported size.
  cfg.cacert_buf = (const unsigned char*)_ca_pem.c_str();
  cfg.cacert_bytes = _ca_pem.length() + 1;
  cfg.clientcert_buf = (const unsigned char*)_cert_pem.c_str();
  cfg.clientcert_bytes = _cert_pem.length() + 1;
  cfg.clientkey_buf = (const unsigned char*)_key_pem.c_str();
  cfg.clientkey_bytes = _key_pem.length() + 1;
  // Portal server cert SAN/CN is typically "takserver", while Integrations host is an FQDN.
  cfg.common_name = "takserver";
  cfg.skip_common_name = false;
  cfg.timeout_ms = 20000;
  // Detect a dead server/NAT path within ~2 min instead of sitting "connected" for days.
  tls_keep_alive_cfg_t ka = {};
  ka.keep_alive_enable = true;
  ka.keep_alive_idle = 60;
  ka.keep_alive_interval = 15;
  ka.keep_alive_count = 4;
  cfg.keep_alive_cfg = &ka;

  int ret = esp_tls_conn_new_sync(_cfg->prefs.tak_host, strlen(_cfg->prefs.tak_host), port, &cfg, TLS);
  if (ret != 1) {
    captureTlsError("TLS handshake failed");
    // Fallback: still verify CA chain, but skip CN/SAN name match
    disconnectTls();
    _tls = esp_tls_init();
    if (!_tls) {
      setError("esp_tls_init failed (retry)");
      return false;
    }
    cfg.common_name = nullptr;
    cfg.skip_common_name = true;
    Serial.println("[TAK] retry TLS with skip_common_name");
    ret = esp_tls_conn_new_sync(_cfg->prefs.tak_host, strlen(_cfg->prefs.tak_host), port, &cfg, TLS);
    if (ret != 1) {
      captureTlsError("TLS failed");
      disconnectTls();
      return false;
    }
  }

  // The handshake used a 20 s socket timeout; a read must never stall the mesh loop that long.
  int fd = -1;
  if (esp_tls_get_conn_sockfd(TLS, &fd) == ESP_OK && fd >= 0) {
    struct timeval tv = {0, 100000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  }

  setError("");
  _backoff_ms = 1000;
  _last_ping_ms = millis();
  _rx_len = 0;
  _presence_due = true;  // first event on the link, so the server routes replies for our uid here
  setState(TakLinkState::Connected);
  Serial.println("[TAK] Connected");
  return true;
}

bool TakClient::drainInbound() {
  if (!_tls) return false;
  for (int i = 0; i < 8; i++) {
    if (esp_tls_get_bytes_avail(TLS) <= 0) {
      int fd = -1;
      if (esp_tls_get_conn_sockfd(TLS, &fd) != ESP_OK || fd < 0) return false;
      fd_set rfds;
      FD_ZERO(&rfds);
      FD_SET(fd, &rfds);
      struct timeval tv = {0, 0};
      int s = select(fd + 1, &rfds, nullptr, nullptr, &tv);
      if (s < 0) return false;
      if (s == 0) return true;
    }
    int n = esp_tls_conn_read(TLS, _rx + _rx_len, sizeof(_rx) - 1 - _rx_len);
    if (n > 0) {
      _rx_len += n;
      _rx[_rx_len] = 0;
      processRx();
      continue;
    }
    if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) return true;
    return false;  // 0 = server closed, <0 = socket error / keepalive timeout
  }
  return true;
}

// ---------- inbound CoT (GeoChat -> MeshCore) ----------

static void xmlUnescape(const char* s, size_t len, char* out, size_t out_len) {
  size_t o = 0;
  for (size_t i = 0; i < len && o + 4 < out_len;) {
    if (s[i] != '&') {
      out[o++] = s[i++];
      continue;
    }
    const char* semi = (const char*)memchr(s + i, ';', len - i < 10 ? len - i : 10);
    if (!semi) {
      out[o++] = s[i++];
      continue;
    }
    size_t el = semi - (s + i) + 1;
    uint32_t cp = 0;
    if (!strncmp(s + i, "&amp;", 5)) cp = '&';
    else if (!strncmp(s + i, "&lt;", 4)) cp = '<';
    else if (!strncmp(s + i, "&gt;", 4)) cp = '>';
    else if (!strncmp(s + i, "&quot;", 6)) cp = '"';
    else if (!strncmp(s + i, "&apos;", 6)) cp = '\'';
    else if (s[i + 1] == '#') cp = (s[i + 2] == 'x') ? strtoul(s + i + 3, nullptr, 16) : strtoul(s + i + 2, nullptr, 10);
    if (!cp) {
      out[o++] = s[i++];
      continue;
    }
    if (cp < 0x80) out[o++] = (char)cp;
    else if (cp < 0x800) { out[o++] = 0xC0 | (cp >> 6); out[o++] = 0x80 | (cp & 0x3F); }
    else if (cp < 0x10000) { out[o++] = 0xE0 | (cp >> 12); out[o++] = 0x80 | ((cp >> 6) & 0x3F); out[o++] = 0x80 | (cp & 0x3F); }
    i += el;
  }
  out[o] = 0;
}

static const char* findTag(const char* xml, const char* name) {
  size_t nl = strlen(name);
  for (const char* p = xml; (p = strchr(p, '<')); p++) {
    if (!strncmp(p + 1, name, nl) && strchr(" \t\r\n/>", p[1 + nl])) return p;
  }
  return nullptr;
}

static bool tagAttr(const char* tag, const char* name, char* out, size_t out_len) {
  out[0] = 0;
  if (!tag) return false;
  const char* end = strchr(tag, '>');
  if (!end) return false;
  size_t nl = strlen(name);
  for (const char* p = tag + 1; p + nl + 2 < end; p++) {
    if (!strchr(" \t\r\n", p[-1]) || strncmp(p, name, nl) || p[nl] != '=') continue;
    char q = p[nl + 1];
    if (q != '"' && q != '\'') continue;
    const char* v = p + nl + 2;
    const char* e = strchr(v, q);
    if (!e) return false;
    xmlUnescape(v, e - v, out, out_len);
    return true;
  }
  return false;
}

void TakClient::processRx() {
  char* end;
  while ((end = strstr(_rx, "</event>"))) {
    end += 8;
    char keep = *end;
    *end = 0;
    handleEvent(_rx);
    *end = keep;
    size_t rest = _rx_len - (end - _rx);
    memmove(_rx, end, rest + 1);
    _rx_len = rest;
  }
  if (_rx_len >= sizeof(_rx) - 1) {
    // an event larger than the buffer (e.g. inline attachments): skip to the next one
    char* next = strstr(_rx + 1, "<event");
    size_t rest = next ? _rx_len - (next - _rx) : 0;
    if (next) memmove(_rx, next, rest + 1);
    _rx_len = rest;
    _rx[_rx_len] = 0;
  }
}

void TakClient::handleEvent(const char* ev) {
  if (!_cfg || !chatEnabled()) return;
  const char* e = findTag(ev, "event");
  char buf[TAK_CHAT_TEXT_LEN * 2];
  if (!e || !tagAttr(e, "type", buf, sizeof(buf)) || strcmp(buf, "b-t-f") != 0) return;
  const char* chat = findTag(e, "__chat");
  if (!chat) return;

  char room[64], id[64], sender[TAK_CALLSIGN_LEN], from_uid[64];
  tagAttr(chat, "chatroom", room, sizeof(room));
  tagAttr(chat, "id", id, sizeof(id));
  tagAttr(chat, "senderCallsign", sender, sizeof(sender));
  tagAttr(findTag(chat, "chatgrp"), "uid0", from_uid, sizeof(from_uid));
  if (!strcmp(from_uid, _gw_uid)) return;

  bool direct = !strcmp(id, _gw_uid) || !strcasecmp(room, _cfg->prefs.chat_callsign);
  for (const char* d = e; !direct && (d = findTag(d + 1, "dest"));) {
    tagAttr(d, "uid", buf, sizeof(buf));
    direct = !strcmp(buf, _gw_uid);
  }

  int ch = -1;
  for (int i = 0; i < TAK_MAX_CHAT && ch < 0; i++) {
    const TakChatChannel& c = _cfg->prefs.chat[i];
    if (c.enabled && c.secret_len && (!strcasecmp(c.room, room) || !strcasecmp(c.room, id))) ch = i;
  }
  for (int i = 0; i < TAK_MAX_CHAT && ch < 0 && direct; i++) {
    if (_cfg->prefs.chat[i].enabled && _cfg->prefs.chat[i].secret_len) ch = i;  // DMs go to the first channel
  }
  if (ch < 0) return;

  const char* rm = findTag(e, "remarks");
  const char* gt = rm ? strchr(rm, '>') : nullptr;
  const char* close = gt ? strstr(gt, "</remarks>") : nullptr;
  if (!close || gt[-1] == '/') return;
  if (_in_count >= IN_SIZE) {
    Serial.println("[CHAT] TAK->mesh queue full, dropping");
    return;
  }
  TakChatIn& m = _in[(_in_head + _in_count) % IN_SIZE];
  m.ch = ch;
  utf8Copy(m.sender, sizeof(m.sender), sender[0] ? sender : "TAK");
  xmlUnescape(gt + 1, close - gt - 1, m.text, sizeof(m.text));
  utf8TrimTail(m.text);
  if (!m.text[0]) return;
  _in_count++;
  Serial.printf("[CHAT] TAK %s -> mesh ch%d: %s\n", m.sender, ch, m.text);
}

bool TakClient::popChatIn(TakChatIn& out) {
  if (!_in_count) return false;
  out = _in[_in_head];
  _in_head = (_in_head + 1) % IN_SIZE;
  _in_count--;
  return true;
}

const char* TakClient::roomFor(int ch) const {
  if (!_cfg) return "?";
  if (ch == TAK_PUBLIC_SLOT) return _cfg->prefs.public_room;
  return (ch >= 0 && ch < TAK_MAX_CHAT) ? _cfg->prefs.chat[ch].room : "?";
}

bool TakClient::chatEnabled() const {
  if (!_cfg) return false;
  if (_cfg->prefs.public_on) return true;
  for (int i = 0; i < TAK_MAX_CHAT; i++) {
    if (_cfg->prefs.chat[i].enabled && _cfg->prefs.chat[i].secret_len) return true;
  }
  return false;
}

void TakClient::noteChat(bool from_mesh, int ch, const char* sender, const char* text) {
  if (from_mesh) chat.mesh_to_tak++;
  else chat.tak_to_mesh++;
  chat.last_ms = millis();
  memmove(&chat.recent[1], &chat.recent[0], sizeof(TakChatMsg) * (TakChatStats::RECENT - 1));
  if (chat.recent_n < TakChatStats::RECENT) chat.recent_n++;
  TakChatMsg& m = chat.recent[0];
  m.from_mesh = from_mesh;
  m.ms = chat.last_ms;
  snprintf(m.text, sizeof(m.text), "[%s] %s: %s", roomFor(ch), sender, text);
  utf8TrimTail(m.text);
}

bool TakClient::queueChat(int ch, const char* sender, const char* text) {
  if (!_cfg || ch < 0 || ch > TAK_PUBLIC_SLOT) return false;
  time_t now = nowUtc();
  if (!now) return false;
  size_t n = TakCot::buildChat(_tx_buf, sizeof(_tx_buf), _gw_uid, roomFor(ch), sender, text,
                               _cfg->prefs.chat_lat, _cfg->prefs.chat_lon, now);
  return n && enqueueXml(_tx_buf, n);
}

bool TakClient::enqueueXml(const char* xml, size_t len) {
  if (!xml || len == 0 || len >= sizeof(_q[0])) return false;
  if (_q_count >= QSIZE) {
    _q_head = (_q_head + 1) % QSIZE;
    _q_count--;
  }
  memcpy(_q[_q_tail], xml, len);
  _q[_q_tail][len] = 0;
  _q_tail = (_q_tail + 1) % QSIZE;
  _q_count++;
  return true;
}

bool TakClient::dequeueXml(char* dest, size_t dest_len, size_t& out_len) {
  if (_q_count == 0) return false;
  size_t len = strlen(_q[_q_head]);
  if (len + 1 > dest_len) return false;
  memcpy(dest, _q[_q_head], len + 1);
  out_len = len;
  _q_head = (_q_head + 1) % QSIZE;
  _q_count--;
  return true;
}

bool TakClient::queuePoint(const TakNodeRecord& node) {
  if (!_cfg) return false;
  if (!TakNodes::passesFilter(node.name, _cfg->prefs)) return false;
  time_t now = nowUtc();
  if (!now) {
    setError("NTP not ready");
    return false;
  }
  size_t n = TakCot::buildPoint(_tx_buf, sizeof(_tx_buf), node, _cfg->prefs, now);
  if (!n) return false;
  return enqueueXml(_tx_buf, n);
}

bool TakClient::queuePing() {
  time_t now = nowUtc();
  if (!now) return false;
  char t[32], st[32];
  struct tm tm_now;
  gmtime_r(&now, &tm_now);
  strftime(t, sizeof(t), "%Y-%m-%dT%H:%M:%SZ", &tm_now);
  time_t stale = now + 60;
  gmtime_r(&stale, &tm_now);
  strftime(st, sizeof(st), "%Y-%m-%dT%H:%M:%SZ", &tm_now);
  int n = snprintf(_tx_buf, sizeof(_tx_buf),
                   "<?xml version=\"1.0\" encoding=\"UTF-8\"?><event version=\"2.0\" uid=\"takPing\" "
                   "type=\"t-x-c-t\" how=\"h-g-i-g-o\" time=\"%s\" start=\"%s\" stale=\"%s\">"
                   "<point lat=\"0\" lon=\"0\" hae=\"0\" ce=\"9999999\" le=\"9999999\"/><detail/></event>",
                   t, t, st);
  if (n <= 0 || (size_t)n >= sizeof(_tx_buf)) return false;
  return enqueueXml(_tx_buf, n);
}

bool TakClient::queueDelete(const char* uid) {
  time_t now = nowUtc();
  if (!now || !uid) return false;
  size_t n = TakCot::buildDelete(_tx_buf, sizeof(_tx_buf), uid, now);
  if (!n) return false;
  return enqueueXml(_tx_buf, n);
}

void TakClient::processRefreshExpire() {
  if (!_nodes || !_cfg) return;
  TakNodeRecord* refresh[TAK_MAX_NODES];
  TakNodeRecord* expire[TAK_MAX_NODES];
  int nr = 0, ne = 0;
  _nodes->collectRefreshAndExpire(millis(), _cfg->prefs.refresh_sec, _cfg->prefs.max_age_sec,
                                  refresh, TAK_MAX_NODES, nr, expire, TAK_MAX_NODES, ne);
  for (int i = 0; i < nr; i++) {
    if (!TakNodes::passesFilter(refresh[i]->name, _cfg->prefs)) continue;
    if (queuePoint(*refresh[i])) _nodes->markSent(refresh[i], millis());
  }
  for (int i = 0; i < ne; i++) {
    if (expire[i]->last_sent_ms) queueDelete(expire[i]->uid);
    expire[i]->valid = false;
  }
}

void TakClient::removeFiltered() {
  if (!_nodes || !_cfg) return;
  for (int i = 0; i < _nodes->count(); i++) {
    TakNodeRecord* n = _nodes->at(i);
    if (n && n->last_sent_ms && !TakNodes::passesFilter(n->name, _cfg->prefs) && queueDelete(n->uid)) {
      n->last_sent_ms = 0;
      Serial.printf("[TAK] removed (name filter): %s\n", n->name);
    }
  }
}

void TakClient::bumpBackoff() {
  setState(TakLinkState::Backoff);
  _backoff_until = millis() + _backoff_ms;
  if (_backoff_ms < 60000) _backoff_ms = min(_backoff_ms * 2, (uint32_t)60000);
  disconnectTls();
}

bool TakClient::testConnection(String& error_out) {
  error_out = "";
  if (WiFi.status() != WL_CONNECTED) {
    error_out = "Wi-Fi not connected";
    return false;
  }
  if (!_cfg->hasTakHost()) {
    error_out = "TAK host not set";
    return false;
  }
  if (!_cfg->hasClientCerts()) {
    error_out = "Upload CA, client cert, and key";
    return false;
  }
  if (!nowUtc()) {
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    for (int i = 0; i < 30 && !nowUtc(); i++) delay(200);
    if (!nowUtc()) {
      error_out = "NTP failed";
      return false;
    }
  }

  // Run a one-shot connect (uses same path as runtime)
  bool was_connected = (_state == TakLinkState::Connected);
  if (!connectTls()) {
    error_out = _last_error;
    bumpBackoff();
    return false;
  }
  char xml[512];
  size_t n = TakCot::buildDelete(xml, sizeof(xml), "meshcore-test-ping", nowUtc());
  if (n) {
    int w = esp_tls_conn_write(TLS, xml, n);
    if (w < 0) {
      captureTlsError("TLS write failed");
      error_out = _last_error;
      disconnectTls();
      return false;
    }
  }
  drainInbound();
  if (!was_connected) {
    // Leave connected for normal operation after a successful test
  }
  error_out = "OK";
  setState(TakLinkState::Connected);
  return true;
}

void TakClient::loop() {
  if (!_cfg) return;

  if (!_cfg->prefs.enabled) {
    if (_state != TakLinkState::Disabled) {
      disconnectTls();
      setState(TakLinkState::Disabled);
    }
    return;
  }

  if (_state == TakLinkState::Backoff) {
    if (millis() < _backoff_until) return;
    setState(TakLinkState::WaitWifi);
  }

  if (_state == TakLinkState::WaitWifi || _state == TakLinkState::Error) {
    if (WiFi.status() == WL_CONNECTED) {
      setState(TakLinkState::WaitNtp);
      configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    }
    return;
  }

  if (_state == TakLinkState::WaitNtp) {
    if (nowUtc()) {
      _ntp_ok = true;
      if (!connectTls()) bumpBackoff();
    }
    return;
  }

  if (_state == TakLinkState::Connecting) return;

  if (_state == TakLinkState::Connected) {
    if (!_tls) {
      setError("connection lost");
      bumpBackoff();
      return;
    }
    if (WiFi.status() != WL_CONNECTED) {
      setError("Wi-Fi lost - reconnecting");
      Serial.println("[TAK] Wi-Fi lost, dropping TLS");
      bumpBackoff();
      return;
    }
    if (millis() >= _next_drain) {
      _next_drain = millis() + 50;
      if (!drainInbound()) {
        captureTlsError("server closed connection");
        bumpBackoff();
        return;
      }
    }
    if (millis() - _last_ping_ms >= 60000UL) {
      _last_ping_ms = millis();
      queuePing();
    }
    if (millis() >= _next_refresh) {
      processRefreshExpire();
      _next_refresh = millis() + 1000;
    }
    if (chatEnabled() && (_presence_due || millis() - _last_presence_ms >= 60000UL)) {
      _presence_due = false;
      _last_presence_ms = millis();
      time_t now = nowUtc();
      size_t n = now ? TakCot::buildPresence(_tx_buf, sizeof(_tx_buf), _gw_uid, _cfg->prefs, now) : 0;
      if (n && esp_tls_conn_write(TLS, _tx_buf, n) != (int)n) {
        captureTlsError("write failed");
        bumpBackoff();
        return;
      }
    }
    size_t len = 0;
    if (dequeueXml(_tx_buf, sizeof(_tx_buf), len)) {
      int w = esp_tls_conn_write(TLS, _tx_buf, len);
      if (w < 0 || (size_t)w != len) {
        captureTlsError("write failed");
        bumpBackoff();
      }
    }
  }
}
