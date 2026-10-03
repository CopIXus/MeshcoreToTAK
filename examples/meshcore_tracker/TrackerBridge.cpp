#include "TrackerBridge.h"

#include <MyMesh.h>
#include <SPIFFS.h>
#include <string.h>

#include "TrackerConfig.h"
#include "TrackerMessage.h"
#include "TrackerMotion.h"

static TrackerMotion motion;
static TrackerConfig cfg;
static uint32_t sequence = 0;
static uint32_t next_sample_ms = 0;
static bool announced = false;
static char line[160];
static uint8_t line_len = 0;
static bool ever_sent = false;
static double sent_lat = 0;
static double sent_lon = 0;
static uint32_t sent_seq = 0;
static uint32_t sent_ms = 0;

static bool channelKeyed(const ChannelDetails& ch) {
  for (int i = 0; i < 32; i++) {
    if (ch.channel.secret[i] != 0) return true;
  }
  return false;
}

static int pickChannel(MyMesh& mesh) {
  ChannelDetails ch;
  if (mesh.getChannel(cfg.channel, ch) && channelKeyed(ch)) return cfg.channel;
  return -1;
}

static int batteryPercent() {
  int mv = board.getBattMilliVolts();
  if (mv < 3000 || mv > 4500) return -1;
  int pct = (mv - 3300) * 100 / 900;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return pct;
}

static bool keptFile(const char* name) {
  return strstr(name, "identity") || strstr(name, "_main.id") || strstr(name, "channels2")
      || strstr(name, "new_prefs") || strstr(name, "prefs.json");
}

static void printFiles() {
  Serial.printf("SPIFFS %u of %u bytes used\n",
                (unsigned)SPIFFS.usedBytes(), (unsigned)SPIFFS.totalBytes());
  File root = SPIFFS.open("/");
  if (!root) return;
  File f = root.openNextFile();
  while (f) {
    Serial.printf("  %s %u\n", f.name(), (unsigned)f.size());
    File next = root.openNextFile();
    f.close();
    f = next;
  }
  root.close();
}

// Advert blobs and the contact list are caches. This radio's SPIFFS has no
// spare room, so those are removed before the callsign is written back.
static void makeRoom(bool drop_contacts) {
  char names[24][40];
  int count = 0;
  File root = SPIFFS.open("/");
  if (root) {
    File f = root.openNextFile();
    while (f && count < 24) {
      const char* name = f.name();
      bool cache = strstr(name, "adv_blob") || strstr(name, "/bl");
      bool contacts = drop_contacts && strstr(name, "contacts");
      if (name && !keptFile(name) && (cache || contacts)) {
        strncpy(names[count], name, sizeof(names[0]) - 1);
        names[count][sizeof(names[0]) - 1] = 0;
        count++;
      }
      File next = root.openNextFile();
      f.close();
      f = next;
    }
    root.close();
  }
  for (int i = 0; i < count; i++) {
    char path[48];
    snprintf(path, sizeof(path), "%s%s", names[i][0] == '/' ? "" : "/", names[i]);
    if (SPIFFS.remove(path)) Serial.printf("removed %s to free settings space\n", path);
  }
}

static void putBytes(uint8_t*& w, const void* src, size_t n) {
  memcpy(w, src, n);
  w += n;
}

static bool writePrefsFile(NodePrefs* prefs) {
  if (prefs->ble_pin == 0) prefs->ble_pin = 123456;
  uint8_t buf[160];
  memset(buf, 0, sizeof(buf));
  uint8_t* w = buf;
  putBytes(w, &prefs->airtime_factor, sizeof(float));
  putBytes(w, prefs->node_name, sizeof(prefs->node_name));
  uint8_t role[4];
  memset(role, 0, sizeof(role));
  size_t role_len = strlen(cfg.role);
  if (role_len > 4) role_len = 4;
  memcpy(role, cfg.role, role_len);
  putBytes(w, role, 4);
  putBytes(w, &prefs->node_lat, sizeof(prefs->node_lat));
  putBytes(w, &prefs->node_lon, sizeof(prefs->node_lon));
  putBytes(w, &prefs->freq, sizeof(prefs->freq));
  putBytes(w, &prefs->sf, sizeof(prefs->sf));
  putBytes(w, &prefs->cr, sizeof(prefs->cr));
  putBytes(w, &prefs->_client_repeat, sizeof(prefs->_client_repeat));
  putBytes(w, &prefs->manual_add_contacts, sizeof(prefs->manual_add_contacts));
  putBytes(w, &prefs->bw, sizeof(prefs->bw));
  putBytes(w, &prefs->tx_power_dbm, sizeof(prefs->tx_power_dbm));
  putBytes(w, &prefs->telemetry_mode_base, sizeof(prefs->telemetry_mode_base));
  putBytes(w, &prefs->telemetry_mode_loc, sizeof(prefs->telemetry_mode_loc));
  putBytes(w, &prefs->telemetry_mode_env, sizeof(prefs->telemetry_mode_env));
  putBytes(w, &prefs->rx_delay_base, sizeof(prefs->rx_delay_base));
  putBytes(w, &prefs->advert_loc_policy, sizeof(prefs->advert_loc_policy));
  putBytes(w, &prefs->multi_acks, sizeof(prefs->multi_acks));
  putBytes(w, &prefs->path_hash_mode, sizeof(prefs->path_hash_mode));
  uint8_t zero = 0;
  putBytes(w, &zero, 1);
  putBytes(w, &prefs->ble_pin, sizeof(prefs->ble_pin));
  putBytes(w, &prefs->buzzer_quiet, sizeof(prefs->buzzer_quiet));
  putBytes(w, &prefs->gps_enabled, sizeof(prefs->gps_enabled));
  putBytes(w, &prefs->gps_interval, sizeof(prefs->gps_interval));
  putBytes(w, &prefs->autoadd_config, sizeof(prefs->autoadd_config));
  putBytes(w, &prefs->autoadd_max_hops, sizeof(prefs->autoadd_max_hops));
  putBytes(w, &prefs->rx_boosted_gain, sizeof(prefs->rx_boosted_gain));
  putBytes(w, prefs->default_scope_name, sizeof(prefs->default_scope_name));
  putBytes(w, prefs->default_scope_key, sizeof(prefs->default_scope_key));
  size_t n = (size_t)(w - buf);

  File f = SPIFFS.open("/new_prefs", "w", true);
  if (!f) return false;
  bool ok = f.write(buf, n) == n;
  f.close();
  if (!ok) return false;
  f = SPIFFS.open("/new_prefs", "r");
  if (!f) return false;
  uint8_t check[32];
  ok = f.seek(4) && f.read(check, 32) == 32 && memcmp(check, prefs->node_name, 32) == 0;
  f.close();
  return ok;
}

static bool saveSettings(NodePrefs* prefs) {
  File cfgf = SPIFFS.open("/tracker.cfg", "w", true);
  if (cfgf) {
    cfgf.printf("ch=%u\nrole=%s\nname=%s\n", (unsigned)cfg.channel, cfg.role, prefs->node_name);
    cfgf.close();
  }
  makeRoom(false);
  if (writePrefsFile(prefs)) return true;
  makeRoom(true);
  if (writePrefsFile(prefs)) return true;
  printFiles();
  return false;
}

static void loadRoleFromPrefs() {
  File prefs = SPIFFS.open("/new_prefs", "r");
  if (!prefs || !prefs.seek(36)) {
    if (prefs) prefs.close();
    return;
  }
  uint8_t raw[4];
  if (prefs.read(raw, 4) == 4) {
    char tmp[5];
    int i = 0;
    while (i < 4 && ((raw[i] >= 'a' && raw[i] <= 'z') || (raw[i] >= '0' && raw[i] <= '9'))) {
      tmp[i++] = (char)raw[i];
    }
    tmp[i] = 0;
    if (i >= 2) memcpy(cfg.role, tmp, sizeof(cfg.role));
  }
  prefs.close();
}

static void loadTrackerFile(NodePrefs* prefs) {
  File f = SPIFFS.open("/tracker.cfg", "r");
  if (!f) {
    loadRoleFromPrefs();
    return;
  }
  char buf[96];
  int n = f.read((uint8_t*)buf, sizeof(buf) - 1);
  f.close();
  if (n <= 0) {
    loadRoleFromPrefs();
    return;
  }
  buf[n] = 0;
  char* saved_name = strstr(buf, "name=");
  if (prefs && saved_name) {
    saved_name += 5;
    int nlen = 0;
    while (saved_name[nlen] && saved_name[nlen] != '\n' && saved_name[nlen] != '\r' && nlen < (int)sizeof(prefs->node_name) - 1) {
      prefs->node_name[nlen] = saved_name[nlen];
      nlen++;
    }
    prefs->node_name[nlen] = 0;
  }
  char* role = strstr(buf, "role=");
  char* slot = strstr(buf, "ch=");
  if (slot) {
    int ch = atoi(slot + 3);
    if (ch >= 0 && ch < MAX_GROUP_CHANNELS) cfg.channel = (uint8_t)ch;
  }
  if (!role) {
    loadRoleFromPrefs();
    return;
  }
  role += 5;
  char tmp[5];
  int i = 0;
  while (role[i] && role[i] != '\n' && role[i] != '\r' && i < 4) {
    char c = role[i];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    tmp[i++] = c;
  }
  tmp[i] = 0;
  if (i >= 2) memcpy(cfg.role, tmp, sizeof(cfg.role));
}

static void printHelp() {
  Serial.println("USB console, 115200. Settings are kept on the device.");
  Serial.println("  status");
  Serial.println("  name <callsign>");
  Serial.println("  role <k9|veh|per|fw|ems|cmd>");
  Serial.println("  channel <slot> <name> <key>");
  Serial.println("  track <slot>");
  Serial.println("Key is 32 or 64 hex characters, or base64. Use the same key as the gateway.");
  Serial.println("A #name with no key uses the MeshCore hashtag channel.");
}

static void printStatus(MyMesh& mesh) {
  NodePrefs* prefs = mesh.getNodePrefs();
  char id[9];
  trackerIdFromPublicKey(mesh.self_id.pub_key, id);
  ChannelDetails ch;
  bool keyed = mesh.getChannel(cfg.channel, ch) && channelKeyed(ch);
  Serial.printf("uid %s  name %s  role %s\n", id, prefs->node_name, cfg.role);
  Serial.printf("radio %.3f MHz  SF%d  BW %.1f  CR%d\n", prefs->freq, prefs->sf, prefs->bw, prefs->cr);
  if (keyed) {
    Serial.printf("tracker slot %u  %s\n", (unsigned)cfg.channel, ch.name);
  } else {
    Serial.printf("tracker slot %u has no key\n", (unsigned)cfg.channel);
  }
  for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
    if (!mesh.getChannel(i, ch) || !channelKeyed(ch)) continue;
    Serial.printf("  channel %d %s%s\n", i, ch.name, i == cfg.channel ? "  <- tracker" : "");
  }
}

static int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int decodeBase64(const char* in, int n, uint8_t* out) {
  static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  int val = 0;
  int valb = -8;
  int len = 0;
  for (int i = 0; i < n; i++) {
    char c = in[i];
    if (c == '=' || c == ' ' || c == '\r' || c == '\n') continue;
    const char* p = strchr(tbl, c);
    if (!p) return -1;
    val = (val << 6) | (int)(p - tbl);
    valb += 6;
    if (valb >= 0) {
      if (len >= 32) return -1;
      out[len++] = (uint8_t)((val >> valb) & 0xFF);
      valb -= 8;
    }
  }
  return len;
}

// 16 or 32 byte key, or 0 if the text is not a key.
static int parseKey(const char* text, uint8_t out[32]) {
  memset(out, 0, 32);
  if (!text || !text[0]) return 0;
  if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text += 2;
  int n = (int)strlen(text);
  bool hex = (n == 32 || n == 64);
  for (int i = 0; hex && i < n; i++) {
    if (hexNibble(text[i]) < 0) hex = false;
  }
  if (hex) {
    for (int i = 0; i < n; i += 2) {
      out[i / 2] = (uint8_t)((hexNibble(text[i]) << 4) | hexNibble(text[i + 1]));
    }
    return n / 2;
  }
  int len = decodeBase64(text, n, out);
  if (len == 16 || len == 32) return len;
  memset(out, 0, 32);
  return 0;
}

static bool roleOk(const char* s) {
  size_t n = strlen(s);
  if (n < 2 || n > 4) return false;
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z');
    if (!ok) return false;
  }
  return true;
}

static char* nextToken(char*& p) {
  while (*p == ' ') p++;
  if (!*p) return nullptr;
  char* start = p;
  while (*p && *p != ' ') p++;
  if (*p) {
    *p = 0;
    p++;
  }
  return start;
}

static void handleLine(MyMesh& mesh, char* text) {
  while (*text == ' ') text++;
  if (!*text) return;

  if (strcmp(text, "help") == 0 || strcmp(text, "?") == 0) {
    printHelp();
    return;
  }
  if (strcmp(text, "status") == 0) {
    printStatus(mesh);
    return;
  }
  if (strncmp(text, "name ", 5) == 0 || strcmp(text, "name") == 0) {
    char* name = text + (strncmp(text, "name ", 5) == 0 ? 5 : 4);
    while (*name == ' ') name++;
    if (!name[0]) {
      Serial.println("usage: name <callsign>");
      return;
    }
    NodePrefs* prefs = mesh.getNodePrefs();
    strncpy(prefs->node_name, name, sizeof(prefs->node_name) - 1);
    prefs->node_name[sizeof(prefs->node_name) - 1] = 0;
    if (!saveSettings(prefs)) {
      Serial.println("name is set until reboot; the settings file could not be updated");
    }
    Serial.printf("name %s\n", prefs->node_name);
    if (strlen(prefs->node_name) > 12) {
      Serial.println("keep the callsign near 12 characters so the fix still fits");
    }
    return;
  }
  if (strncmp(text, "role ", 5) == 0 || strcmp(text, "role") == 0) {
    char* role = text + 4;
    while (*role == ' ') role++;
    for (char* p = role; *p; p++) {
      if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
    }
    if (!roleOk(role)) {
      Serial.println("role is 2 to 4 letters or digits, such as k9 or veh");
      return;
    }
    memset(cfg.role, 0, sizeof(cfg.role));
    memcpy(cfg.role, role, strlen(role));
    if (!saveSettings(mesh.getNodePrefs())) {
      Serial.println("role is set until reboot; the settings file could not be updated");
    }
    Serial.printf("role %s\n", cfg.role);
    return;
  }
  if (strncmp(text, "track ", 6) == 0 || strcmp(text, "track") == 0) {
    char* arg = text + 5;
    while (*arg == ' ') arg++;
    if (!arg[0]) {
      Serial.println("usage: track <slot>");
      return;
    }
    int slot = atoi(arg);
    ChannelDetails ch;
    if (slot < 0 || slot >= MAX_GROUP_CHANNELS || !mesh.getChannel(slot, ch) || !channelKeyed(ch)) {
      Serial.println("that slot has no key. Use channel <slot> <name> <key>");
      return;
    }
    cfg.channel = (uint8_t)slot;
    saveSettings(mesh.getNodePrefs());
    Serial.printf("tracker slot %d %s\n", slot, ch.name);
    return;
  }
  if (strncmp(text, "channel ", 8) == 0) {
    char* p = text + 8;
    char* slot_s = nextToken(p);
    char* name = nextToken(p);
    while (*p == ' ') p++;
    if (!slot_s || !name) {
      Serial.println("usage: channel <slot> <name> <key>");
      return;
    }
    int slot = atoi(slot_s);
    if (slot < 0 || slot >= MAX_GROUP_CHANNELS) {
      Serial.printf("slot must be 0 to %d\n", MAX_GROUP_CHANNELS - 1);
      return;
    }
    ChannelDetails ch;
    memset(&ch, 0, sizeof(ch));
    strncpy(ch.name, name, sizeof(ch.name) - 1);
    int key_len = 0;
    if (p[0]) {
      key_len = parseKey(p, ch.channel.secret);
      if (key_len != 16 && key_len != 32) {
        Serial.println("key must be 32 or 64 hex characters, or base64");
        return;
      }
    } else if (name[0] == '#') {
      mesh::Utils::sha256(ch.channel.secret, 16, (const uint8_t*)name, (int)strlen(name));
      key_len = 16;
    } else {
      Serial.println("private channel needs a key. Example: channel 1 TNTAK <32 hex chars>");
      return;
    }
    if (key_len == 16) memset(ch.channel.secret + 16, 0, 16);
    if (!mesh.setChannel(slot, ch)) {
      Serial.println("could not store that channel");
      return;
    }
    mesh.saveChannels();
    cfg.channel = (uint8_t)slot;
    saveSettings(mesh.getNodePrefs());
    Serial.printf("tracker slot %d %s, %d-byte key saved\n", slot, ch.name, key_len);
    return;
  }
  Serial.println("unknown command. Type help");
}

static void pollConsole(MyMesh& mesh) {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r' || c == '\n') {
      if (line_len == 0) continue;
      line[line_len] = 0;
      line_len = 0;
      handleLine(mesh, line);
    } else if (line_len < sizeof(line) - 1) {
      line[line_len++] = c;
    }
  }
}

void trackerBridgeBegin(MyMesh& mesh) {
  cfg = trackerDefaults();
  loadTrackerFile(mesh.getNodePrefs());
  motion.setConfig(cfg.motion);

  NodePrefs* prefs = mesh.getNodePrefs();
  bool changed = false;
  if (!prefs->gps_enabled) {
    prefs->gps_enabled = 1;
    changed = true;
  }
  if (prefs->gps_interval == 0 || prefs->gps_interval > 2) {
    prefs->gps_interval = 1;
    changed = true;
  }
  if (prefs->advert_loc_policy != ADVERT_LOC_NONE) {
    prefs->advert_loc_policy = ADVERT_LOC_NONE;
    changed = true;
  }
  // Unconfigured companions boot on the EU preset. This gateway uses the US one.
  // A frequency already saved from the MeshCore app is left as the user set it.
  if (prefs->freq > 869.0f && prefs->freq < 870.0f && prefs->sf == 8) {
    prefs->freq = 910.525f;
    prefs->bw = 62.5f;
    prefs->sf = 7;
    prefs->cr = 5;
    changed = true;
    Serial.println("radio was the EU default; set to 910.525 MHz SF7");
  }
  mesh.applyGpsPrefs();
  if (changed) radio_driver.setParams(prefs->freq, prefs->bw, prefs->sf, prefs->cr);
  if (!saveSettings(prefs)) {
    Serial.println("could not store tracker settings");
  }

  Serial.println("MeshCoreTracker");
  printStatus(mesh);
  Serial.println("advert location off, GPS on");
  printHelp();
}

void trackerBridgeLoop(MyMesh& mesh) {
  pollConsole(mesh);

  uint32_t now_ms = millis();
  if ((int32_t)(now_ms - next_sample_ms) < 0) return;
  next_sample_ms = now_ms + 1000;

  LocationProvider* gps = sensors.getLocationProvider();
  bool valid = gps && gps->isValid();
  TrackerFix fix;
  fix.valid = valid;
  fix.lat = valid ? sensors.node_lat : 0;
  fix.lon = valid ? sensors.node_lon : 0;
  fix.speed_mps = 0;
  TrackerDecision decision = motion.onFix(fix, now_ms / 1000);
  if (!decision.send) {
    if (!announced && !valid) {
      Serial.println("waiting for GPS fix");
      announced = true;
    }
    return;
  }

  int slot = pickChannel(mesh);
  ChannelDetails ch;
  if (slot < 0 || !mesh.getChannel(slot, ch)) {
    Serial.printf("fix ready. slot %u has no key. Type: channel %u <name> <key>\n",
                  (unsigned)cfg.channel, (unsigned)cfg.channel);
    return;
  }

  uint32_t unix_s = rtc_clock.getCurrentTime();
  if (unix_s > 1700000000UL && unix_s > sequence) sequence = unix_s;
  sequence++;

  NodePrefs* prefs = mesh.getNodePrefs();
  TrackerReport report;
  memset(&report, 0, sizeof(report));
  trackerIdFromPublicKey(mesh.self_id.pub_key, report.id);
  memcpy(report.role, cfg.role, sizeof(report.role));
  report.lat = sensors.node_lat;
  report.lon = sensors.node_lon;
  report.altitude_m = (float)sensors.node_altitude;
  report.has_altitude = true;
  report.battery_pct = batteryPercent();
  report.stale_sec = decision.stale_s;
  report.sequence = sequence;

  char body[128];
  size_t n = trackerFormatFix(body, sizeof(body), report);
  if (n == 0) {
    Serial.println("tracker packet rejected by formatter");
    return;
  }

  uint32_t stamp = unix_s > 1700000000UL ? unix_s : (uint32_t)(now_ms / 1000);
  if (!mesh.sendGroupMessage(stamp, ch.channel, prefs->node_name, body, (int)n)) {
    Serial.println("tracker send failed");
    return;
  }
  announced = true;
  ever_sent = true;
  sent_lat = report.lat;
  sent_lon = report.lon;
  sent_seq = report.sequence;
  sent_ms = now_ms;
  Serial.printf("sent slot %d %s: %s\n", slot, prefs->node_name, body);
}

void trackerBridgeCopyStatus(MyMesh& mesh, TrackerScreenInfo* out) {
  memset(out, 0, sizeof(*out));
  memcpy(out->role, cfg.role, sizeof(out->role));
  trackerIdFromPublicKey(mesh.self_id.pub_key, out->uid);
  out->slot = cfg.channel;
  ChannelDetails ch;
  if (mesh.getChannel(cfg.channel, ch) && channelKeyed(ch)) {
    out->keyed = true;
    strncpy(out->channel, ch.name, sizeof(out->channel) - 1);
  }
  out->sent = ever_sent;
  out->lat = sent_lat;
  out->lon = sent_lon;
  out->sequence = sent_seq;
  if (ever_sent) out->age_s = (millis() - sent_ms) / 1000;
}
