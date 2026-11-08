#include "TrackerBridge.h"

#include <MyMesh.h>
#include <SPIFFS.h>
#include <helpers/TransportKeyStore.h>
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
static uint32_t gps_bytes_seen = 0;
static uint32_t gps_marks_seen = 0;
static uint32_t gps_quiet_since_ms = 0;
static uint32_t gps_report_ms = 0;
static uint8_t gps_baud_i = 0;
static const uint32_t gps_bauds[] = {9600, 115200, 38400, 57600};
static double sent_lat = 0;
static double sent_lon = 0;
static uint32_t sent_seq = 0;
static uint32_t sent_ms = 0;
static bool have_fix = false;
static const char* problem = "";
static uint32_t problem_report_ms = 0;
static uint32_t fixes_sent = 0;
static uint32_t gps_forced = 0;
static uint32_t fix_lost_count = 0;
static uint32_t no_fix_since_ms = 0;
static uint32_t flood_heard = 0;
static uint32_t echo_count = 0;
static uint8_t echo_hops = 0;
static uint32_t echo_age_s = 0;
static uint8_t scoped_misses = 0;
static bool prev_scoped = false;
static bool scope_warned = false;
static bool last_tx_echoed = true;
static uint8_t last_tx[184];
static uint16_t last_tx_len = 0;
static uint32_t last_tx_ms = 0;

enum TripEv : uint8_t {
  EV_FIX = 1,
  EV_LOST = 2,
  EV_SEND = 3,
  EV_ECHO = 4,
  EV_BAUD = 5,
  EV_BATT = 6,
  EV_SCOPE = 7,
  EV_FAIL = 8,
  EV_GPS_FORCED = 9,
};

struct TripEnt {
  uint32_t t;
  uint8_t ev;
  uint32_t v;
};
static TripEnt trip[64];
static uint8_t trip_n = 0;
static uint8_t trip_i = 0;

static void tripLog(uint8_t ev, uint32_t v) {
  trip[trip_i].t = millis() / 1000;
  trip[trip_i].ev = ev;
  trip[trip_i].v = v;
  trip_i = (uint8_t)((trip_i + 1) % 64);
  if (trip_n < 64) trip_n++;
}

static const char* tripName(uint8_t ev) {
  switch (ev) {
    case EV_FIX: return "fix";
    case EV_LOST: return "fix lost";
    case EV_SEND: return "sent";
    case EV_ECHO: return "echo";
    case EV_BAUD: return "gps baud";
    case EV_BATT: return "battery mV";
    case EV_SCOPE: return "scope";
    case EV_FAIL: return "send failed";
    case EV_GPS_FORCED: return "gps forced on";
    default: return "event";
  }
}

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

static bool cacheFile(const char* name, bool drop_contacts) {
  if (!name || keptFile(name)) return false;
  if (strstr(name, "adv_blob") || strstr(name, "/bl") || (drop_contacts && strstr(name, "contacts"))) return true;
  const char* base = strrchr(name, '/');
  base = base ? base + 1 : name;
  int n = 0;
  for (; base[n]; n++) {
    char c = base[n];
    bool hex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
    if (!hex) return false;
  }
  return n == 16;
}

// Advert blobs fill this radio's SPIFFS, so a settings write cannot open.
// Those cached files are removed until there is room. Channels and identity stay.
static void makeRoom(bool drop_contacts) {
  int removed = 0;
  for (int pass = 0; pass < 30; pass++) {
    size_t free_bytes = SPIFFS.totalBytes() - SPIFFS.usedBytes();
    if (free_bytes >= 8192) break;
    char paths[16][48];
    int count = 0;
    File root = SPIFFS.open("/");
    if (!root) break;
    File f = root.openNextFile();
    while (f && count < 16) {
      const char* full = f.path();
      if (full && !f.isDirectory() && cacheFile(full, drop_contacts)) {
        strncpy(paths[count], full, sizeof(paths[0]) - 1);
        paths[count][sizeof(paths[0]) - 1] = 0;
        count++;
      }
      File next = root.openNextFile();
      f.close();
      f = next;
    }
    root.close();
    if (count == 0) break;
    int removed_now = 0;
    for (int i = 0; i < count; i++) {
      if (SPIFFS.remove(paths[i]) || SPIFFS.rmdir(paths[i])) removed_now++;
    }
    removed += removed_now;
    if (removed_now == 0) break;
  }
  if (removed > 0) Serial.printf("removed %d cached files to free settings space\n", removed);
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

static bool writeTrackerCfg(NodePrefs* prefs) {
  char body[160];
  int n = snprintf(body, sizeof(body), "ch=%u\nrole=%s\nname=%s\nmv=%u\nstill=%u\nstale=%u\n",
                   (unsigned)cfg.channel, cfg.role, prefs->node_name,
                   (unsigned)cfg.motion.move_interval_s,
                   (unsigned)cfg.motion.stationary_interval_s,
                   (unsigned)cfg.motion.stale_min_s);
  if (n <= 0 || n >= (int)sizeof(body)) return false;
  File cfgf = SPIFFS.open("/tracker.cfg", "w", true);
  if (!cfgf) {
    SPIFFS.remove("/tracker.cfg");
    cfgf = SPIFFS.open("/tracker.cfg", "w", true);
  }
  if (!cfgf) return false;
  bool ok = cfgf.write((const uint8_t*)body, (size_t)n) == (size_t)n;
  cfgf.close();
  return ok;
}

static bool saveSettings(NodePrefs* prefs) {
  if (writeTrackerCfg(prefs)) return true;
  makeRoom(false);
  if (writeTrackerCfg(prefs)) return true;
  makeRoom(true);
  if (writeTrackerCfg(prefs)) return true;
  printFiles();
  return false;
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
      tmp[i] = (char)raw[i];
      i++;
    }
    tmp[i] = 0;
    if (i >= 2) memcpy(cfg.role, tmp, sizeof(cfg.role));
  }
  prefs.close();
}

static void applySeconds(const char* buf, const char* key, uint16_t* dest) {
  const char* p = strstr(buf, key);
  if (!p) return;
  int v = atoi(p + strlen(key));
  if (v >= 5 && v <= 3600) *dest = (uint16_t)v;
}

static void loadTrackerFile(NodePrefs* prefs) {
  File f = SPIFFS.open("/tracker.cfg", "r");
  if (!f) {
    loadRoleFromPrefs();
    return;
  }
  char buf[160];
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
  applySeconds(buf, "mv=", &cfg.motion.move_interval_s);
  applySeconds(buf, "still=", &cfg.motion.stationary_interval_s);
  applySeconds(buf, "stale=", &cfg.motion.stale_min_s);
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
  // The formatter refuses a bad role, which would silently block every fix.
  if (roleOk(tmp)) memcpy(cfg.role, tmp, sizeof(cfg.role));
  else Serial.printf("role '%s' in tracker.cfg is not valid, using %s\n", tmp, cfg.role);
}

static uint16_t staleSeconds(uint16_t interval_s) {
  float v = (float)interval_s * cfg.motion.stale_multiplier;
  if (v < (float)cfg.motion.stale_min_s) v = (float)cfg.motion.stale_min_s;
  if (v < 5.0f) v = 5.0f;
  if (v > 3600.0f) v = 3600.0f;
  return (uint16_t)(v + 0.5f);
}

static bool scopeNameOk(const char* name) {
  if (!name || !name[0]) return false;
  size_t n = strlen(name);
  if (n > 29) return false;
  for (size_t i = 0; i < n; i++) {
    char c = name[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    if (!ok) return false;
  }
  return true;
}

// Empty name clears the scope. A region name is stored the way a repeater
// derives it: the transport key is the hash of '#' plus the name.
static bool setFloodScope(MyMesh& mesh, const char* name) {
  NodePrefs* prefs = mesh.getNodePrefs();
  if (!name || !name[0] || strcmp(name, "off") == 0 || strcmp(name, "*") == 0) {
    prefs->default_scope_name[0] = 0;
    memset(prefs->default_scope_key, 0, sizeof(prefs->default_scope_key));
    mesh.savePrefs();
    return true;
  }
  if (name[0] == '#') name++;
  if (name[0] == '$') return false;
  if (!scopeNameOk(name)) return false;
  char tagged[32];
  snprintf(tagged, sizeof(tagged), "#%s", name);
  TransportKeyStore store;
  TransportKey key;
  store.getAutoKeyFor(0, tagged, key);
  strncpy(prefs->default_scope_name, name, sizeof(prefs->default_scope_name) - 1);
  prefs->default_scope_name[sizeof(prefs->default_scope_name) - 1] = 0;
  memcpy(prefs->default_scope_key, key.key, sizeof(prefs->default_scope_key));
  mesh.savePrefs();
  return true;
}

static void printHelp() {
  Serial.println("USB console, 115200. Settings are kept on the device.");
  Serial.println("  name <callsign>");
  Serial.println("  role <k9|veh|per|fw|ems|cmd>");
  Serial.println("  move <seconds>     how often to send while moving");
  Serial.println("  still <seconds>    how often to send while stopped");
  Serial.println("  stale <seconds>    minimum time the marker stays fresh");
  Serial.println("  scope <region>     flood region, exact case. scope off sends an unscoped flood");
  Serial.println("  status             counters. status log prints the trip buffer");
  Serial.println("  channel <slot> <name> <key>");
  Serial.println("  track <slot>");
  Serial.println("Seconds are 5 to 3600. The marker stays fresh for twice the send interval, or for stale when that is longer.");
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
  Serial.printf("moving every %us, marker fresh %us\n",
                (unsigned)cfg.motion.move_interval_s, (unsigned)staleSeconds(cfg.motion.move_interval_s));
  Serial.printf("stopped every %us, marker fresh %us\n",
                (unsigned)cfg.motion.stationary_interval_s, (unsigned)staleSeconds(cfg.motion.stationary_interval_s));
  Serial.printf("radio %.3f MHz  SF%d  BW %.1f  CR%d\n", prefs->freq, prefs->sf, prefs->bw, prefs->cr);
  if (prefs->default_scope_name[0]) {
    Serial.printf("scope %s\n", prefs->default_scope_name);
    Serial.printf("scope %s set: only repeaters that have region %s with flood allowed will carry these fixes. use scope off for an unscoped flood\n",
                  prefs->default_scope_name, prefs->default_scope_name);
  } else {
    Serial.println("scope none");
  }
  Serial.printf("tx %d dBm  path hash %u\n", prefs->tx_power_dbm, (unsigned)prefs->path_hash_mode);
  LocationProvider* gps = sensors.getLocationProvider();
  uint32_t no_fix_s = have_fix ? 0 : (millis() - no_fix_since_ms) / 1000;
  Serial.printf("gps %s  sats %ld  no-fix %lus  lost %lu  forced-on %lu  fixes sent %lu\n",
                have_fix ? "fix" : "no fix", gps ? gps->satellitesCount() : 0, (unsigned long)no_fix_s,
                (unsigned long)fix_lost_count, (unsigned long)gps_forced, (unsigned long)fixes_sent);
  Serial.printf("radio tx flood %lu  airtime %lu ms\n", (unsigned long)mesh.getNumSentFlood(),
                (unsigned long)mesh.getTotalAirTime());
  Serial.printf("echoes %lu  last hops %u  last age %lus\n", (unsigned long)echo_count, (unsigned)echo_hops,
                (unsigned long)echo_age_s);
  Serial.printf("flood heard with hops %lu\n", (unsigned long)flood_heard);
  if (problem[0]) Serial.printf("last problem: %s\n", problem);
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

static void printTripLog() {
  if (trip_n == 0) {
    Serial.println("trip log empty");
    return;
  }
  int start = trip_n < 64 ? 0 : trip_i;
  for (int n = 0; n < trip_n; n++) {
    const TripEnt& e = trip[(start + n) % 64];
    Serial.printf("  %lus  %s  %lu\n", (unsigned long)e.t, tripName(e.ev), (unsigned long)e.v);
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
  if (strcmp(text, "status") == 0 || strncmp(text, "status ", 7) == 0) {
    printStatus(mesh);
    if (strstr(text, "log")) printTripLog();
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
  if (strncmp(text, "move ", 5) == 0 || strcmp(text, "move") == 0 ||
      strncmp(text, "still ", 6) == 0 || strcmp(text, "still") == 0 ||
      strncmp(text, "stale ", 6) == 0 || strcmp(text, "stale") == 0) {
    bool is_move = strncmp(text, "move", 4) == 0;
    bool is_still = strncmp(text, "still", 5) == 0;
    char* arg = text;
    while (*arg && *arg != ' ') arg++;
    while (*arg == ' ') arg++;
    int v = atoi(arg);
    const char* what = is_move ? "move" : (is_still ? "still" : "stale");
    if (!arg[0] || v < 5 || v > 3600) {
      Serial.printf("%s is 5 to 3600 seconds\n", what);
      return;
    }
    if (is_move) cfg.motion.move_interval_s = (uint16_t)v;
    else if (is_still) cfg.motion.stationary_interval_s = (uint16_t)v;
    else cfg.motion.stale_min_s = (uint16_t)v;
    motion.setConfig(cfg.motion);
    if (!saveSettings(mesh.getNodePrefs())) {
      Serial.println("timing is set until reboot; the settings file could not be updated");
    }
    Serial.printf("moving every %us, marker fresh %us\n",
                  (unsigned)cfg.motion.move_interval_s, (unsigned)staleSeconds(cfg.motion.move_interval_s));
    Serial.printf("stopped every %us, marker fresh %us\n",
                  (unsigned)cfg.motion.stationary_interval_s, (unsigned)staleSeconds(cfg.motion.stationary_interval_s));
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
  if (strncmp(text, "scope ", 6) == 0 || strcmp(text, "scope") == 0) {
    char* arg = text + 5;
    while (*arg == ' ') arg++;
    if (!arg[0]) {
      NodePrefs* prefs = mesh.getNodePrefs();
      Serial.printf("scope %s\n", prefs->default_scope_name[0] ? prefs->default_scope_name : "none");
      return;
    }
    if (arg[0] == '$' || (arg[0] == '#' && arg[1] == '$')) {
      Serial.println("private $ regions use a key this radio cannot derive. Use scope off, or a public region name");
      return;
    }
    if (!setFloodScope(mesh, arg)) {
      Serial.println("scope is a region name (letters, digits, - or _), exact case, or off");
      return;
    }
    tripLog(EV_SCOPE, mesh.getNodePrefs()->default_scope_name[0] ? 1 : 0);
    scoped_misses = 0;
    scope_warned = false;
    NodePrefs* prefs = mesh.getNodePrefs();
    Serial.printf("scope %s\n", prefs->default_scope_name[0] ? prefs->default_scope_name : "none");
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
      Serial.println("private channel needs a key. Example: channel 1 Track <32 hex chars>");
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

static void sendUbx(uint8_t cls, uint8_t id, const uint8_t* payload, uint16_t len) {
  uint8_t hdr[4] = {cls, id, (uint8_t)len, (uint8_t)(len >> 8)};
  uint8_t a = 0, b = 0;
  for (int i = 0; i < 4; i++) { a = (uint8_t)(a + hdr[i]); b = (uint8_t)(b + a); }
  for (uint16_t i = 0; i < len; i++) { a = (uint8_t)(a + payload[i]); b = (uint8_t)(b + a); }
  Serial1.write(0xB5);
  Serial1.write(0x62);
  Serial1.write(hdr, 4);
  if (len) Serial1.write(payload, len);
  Serial1.write(a);
  Serial1.write(b);
}

static void sendNmea(const char* body) {
  uint8_t cs = 0;
  for (const char* p = body; *p; ++p) cs ^= (uint8_t)*p;
  Serial1.print('$');
  Serial1.print(body);
  Serial1.print('*');
  if (cs < 16) Serial1.print('0');
  Serial1.print(cs, HEX);
  Serial1.print("\r\n");
}

static void configureGps() {
  // Stay in continuous tracking and ask for the sentences a fix is read from.
  // u-blox, MediaTek, and AT6558 each ignore the commands they do not understand.
  const uint8_t nmea_ids[] = {0x00, 0x04, 0x03, 0x02};  // GGA, RMC, GSV, GSA
  for (uint8_t id : nmea_ids) {
    uint8_t msg[8] = {0xF0, id, 0, 1, 0, 0, 0, 0};
    sendUbx(0x06, 0x01, msg, 8);
    delay(20);
  }
  uint8_t rxm[2] = {0x00, 0x00};
  sendUbx(0x06, 0x11, rxm, 2);
  delay(20);
  uint8_t rate[6] = {0xE8, 0x03, 0x01, 0x00, 0x01, 0x00};
  sendUbx(0x06, 0x08, rate, 6);
  delay(20);
  sendNmea("PMTK225,0");
  sendNmea("PMTK314,0,1,0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0");
  sendNmea("PMTK353,1,1,1,0,0");
  sendNmea("PCAS04,7");
  sendNmea("PCAS03,1,0,1,1,1,0,0,0,0,0,,,0,0");
  Serial.println("gps set to continuous tracking");
}

static void openGpsUart() {
  // GPIO 34 is input-only, so the ESP receive pin stays PIN_GPS_TX.
  Serial1.end();
  delay(20);
  Serial1.setPins(PIN_GPS_TX, PIN_GPS_RX);
  Serial1.begin(gps_bauds[gps_baud_i]);
  Serial.printf("gps listening at %lu baud (esp rx %d tx %d)\n",
                (unsigned long)gps_bauds[gps_baud_i], PIN_GPS_TX, PIN_GPS_RX);
  tripLog(EV_BAUD, gps_bauds[gps_baud_i]);
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
  // A build can name a flood region with -D TRACKER_DEFAULT_SCOPE=\"tn-east\".
  // An empty scope stays empty, so a radio is not silently moved onto someone else's region.
#ifdef TRACKER_DEFAULT_SCOPE
  if (!prefs->default_scope_name[0]) {
    if (setFloodScope(mesh, TRACKER_DEFAULT_SCOPE)) {
      Serial.printf("scope was empty; set to %s\n", TRACKER_DEFAULT_SCOPE);
    }
  }
#endif
  if (!saveSettings(prefs)) {
    Serial.println("could not store tracker settings");
  }

  Serial.println("MeshCoreTracker");
  printStatus(mesh);
  Serial.println("advert location off, GPS on");
  no_fix_since_ms = millis();
  gps_quiet_since_ms = no_fix_since_ms;
  gps_report_ms = gps_quiet_since_ms + 15000;
  Serial.printf("gps listening at %lu baud (esp rx %d tx %d)\n",
                (unsigned long)gps_bauds[0], PIN_GPS_TX, PIN_GPS_RX);
  configureGps();
  printHelp();
}

static void keepGpsOn(MyMesh& mesh) {
  NodePrefs* prefs = mesh.getNodePrefs();
  bool off = !prefs->gps_enabled;
  for (int i = 0; i < sensors.getNumSettings(); i++) {
    const char* name = sensors.getSettingName(i);
    if (!name || strcmp(name, "gps") != 0) continue;
    const char* v = sensors.getSettingValue(i);
    if (v && v[0] == '0') off = true;
  }
  if (!off) return;
  static uint32_t last_force_ms = 0;
  if (last_force_ms && (int32_t)(millis() - last_force_ms) < 5000) return;
  last_force_ms = millis();
  prefs->gps_enabled = 1;
  sensors.setSettingValue("gps", "1");
  mesh.savePrefs();
  gps_forced++;
  tripLog(EV_GPS_FORCED, gps_forced);
  Serial.println("gps was switched off; turned back on");
}

void trackerBridgeLoop(MyMesh& mesh) {
  pollConsole(mesh);
  keepGpsOn(mesh);

  uint32_t now_ms = millis();
  if ((int32_t)(now_ms - next_sample_ms) < 0) return;
  next_sample_ms = now_ms + 1000;

  LocationProvider* gps = sensors.getLocationProvider();
  bool valid = gps && gps->isValid();
  uint32_t bytes = gps ? gps->rxBytes() : 0;
  uint32_t marks = gps ? gps->nmeaMarks() : 0;
  if (marks != gps_marks_seen) {
    if (gps_marks_seen == 0) {
      Serial.printf("gps is talking (%lu nmea)\n", (unsigned long)marks);
    }
    gps_marks_seen = marks;
    gps_bytes_seen = bytes;
    gps_quiet_since_ms = now_ms;
  }
  if (!valid && marks == gps_marks_seen && (int32_t)(now_ms - gps_quiet_since_ms) > 12000) {
    gps_baud_i++;
    if (gps_baud_i >= (uint8_t)(sizeof(gps_bauds) / sizeof(gps_bauds[0]))) {
      gps_baud_i = 0;
    }
    openGpsUart();
    gps_quiet_since_ms = now_ms;
  }
  if (!valid && (int32_t)(now_ms - gps_report_ms) >= 0) {
    gps_report_ms = now_ms + 15000;
    Serial.printf("gps no fix  sats %ld  gga %lu rmc %lu gsv %lu  %s\n",
                  gps ? gps->satellitesCount() : 0,
                  gps ? (unsigned long)gps->ggaCount() : 0,
                  gps ? (unsigned long)gps->rmcCount() : 0,
                  gps ? (unsigned long)gps->gsvCount() : 0,
                  gps ? gps->lastSentence() : "");
  }
  if (valid && !have_fix) {
    Serial.printf("gps fix  sats %ld  %.5f %.5f  up %lus\n", gps->satellitesCount(),
                  gps->getLatitude() / 1000000.0, gps->getLongitude() / 1000000.0,
                  (unsigned long)(now_ms / 1000));
    tripLog(EV_FIX, (uint32_t)gps->satellitesCount());
  } else if (!valid && have_fix) {
    Serial.printf("gps fix lost  up %lus\n", (unsigned long)(now_ms / 1000));
    tripLog(EV_LOST, gps ? (uint32_t)gps->satellitesCount() : 0);
    fix_lost_count++;
    no_fix_since_ms = now_ms;
  }
  have_fix = valid;

  // sensors.node_lat only refreshes every gps_interval, so it can still be 0,0
  // in the second the receiver first reports a fix. Read the receiver directly.
  TrackerFix fix;
  fix.valid = valid;
  fix.lat = valid ? gps->getLatitude() / 1000000.0 : 0;
  fix.lon = valid ? gps->getLongitude() / 1000000.0 : 0;
  fix.speed_mps = 0;
  fix.has_speed = valid && gps->hasSpeed();
  if (fix.has_speed) fix.speed_mps = gps->getSpeedMps();
  if (valid && fix.lat == 0 && fix.lon == 0) fix.valid = false;

  // onFix records a send as done. Keep the prior state so a fix that does not
  // go out is offered again next second instead of after the stopped interval.
  TrackerMotion before = motion;
  TrackerDecision decision = motion.onFix(fix, now_ms / 1000);
  if (!decision.send) {
    if (!announced && !valid) {
      Serial.println("waiting for GPS fix");
      announced = true;
    }
    return;
  }
  bool report_problem = (int32_t)(now_ms - problem_report_ms) >= 0;

  int slot = pickChannel(mesh);
  ChannelDetails ch;
  if (slot < 0 || !mesh.getChannel(slot, ch)) {
    motion = before;
    problem = "no channel key";
    if (report_problem) {
      problem_report_ms = now_ms + 15000;
      Serial.printf("fix ready. slot %u has no key. Type: channel %u <name> <key>\n",
                    (unsigned)cfg.channel, (unsigned)cfg.channel);
    }
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
  report.lat = fix.lat;
  report.lon = fix.lon;
  report.altitude_m = (float)(gps->getAltitude() / 1000.0);
  report.has_altitude = true;
  report.battery_pct = batteryPercent();
  report.stale_sec = decision.stale_s;
  report.sequence = sequence;

  char body[128];
  size_t n = trackerFormatFix(body, sizeof(body), report);
  if (n == 0) {
    motion = before;
    problem = "fix rejected";
    if (report_problem) {
      problem_report_ms = now_ms + 15000;
      Serial.printf("tracker packet rejected: role '%s' lat %.5f lon %.5f alt %.0f stale %u\n",
                    report.role, report.lat, report.lon, report.altitude_m,
                    (unsigned)report.stale_sec);
    }
    return;
  }

  uint32_t stamp = unix_s > 1700000000UL ? unix_s : (uint32_t)(now_ms / 1000);
  if (!mesh.sendGroupMessage(stamp, ch.channel, prefs->node_name, body, (int)n)) {
    motion = before;
    problem = "send failed";
    if (report_problem) {
      problem_report_ms = now_ms + 15000;
      Serial.println("tracker send failed, retrying");
    }
    tripLog(EV_FAIL, sequence);
    return;
  }
  if (prev_scoped && last_tx_len && !last_tx_echoed && scoped_misses < 255) scoped_misses++;
  prev_scoped = prefs->default_scope_name[0] != 0;
  if (!prev_scoped) {
    scoped_misses = 0;
    scope_warned = false;
  } else if (scoped_misses >= 3 && flood_heard > 0 && !scope_warned) {
    scope_warned = true;
    Serial.println("scope is set and repeaters are not echoing these fixes, while other flood traffic is arriving. Try: scope off");
  }
  fixes_sent++;
  tripLog(EV_SEND, sequence);
  int mv = board.getBattMilliVolts();
  if (mv > 0) tripLog(EV_BATT, (uint32_t)mv);
  problem = "";
  problem_report_ms = now_ms;
  announced = true;
  ever_sent = true;
  sent_lat = report.lat;
  sent_lon = report.lon;
  sent_seq = report.sequence;
  sent_ms = now_ms;
  const char* pace = decision.state == TrackerMotionState::Moving ? "moving" : "stopped";
  Serial.printf("sent %s every %us slot %d scope %s %s: %s\n",
                pace, (unsigned)decision.interval_s, slot,
                prefs->default_scope_name[0] ? prefs->default_scope_name : "none",
                prefs->node_name, body);
}

void trackerBridgeOnTx(mesh::Packet* pkt) {
  if (!pkt || pkt->getPayloadType() != PAYLOAD_TYPE_GRP_TXT) return;
  uint16_t n = pkt->payload_len;
  if (n > sizeof(last_tx)) n = sizeof(last_tx);
  memcpy(last_tx, pkt->payload, n);
  last_tx_len = n;
  last_tx_ms = millis();
  last_tx_echoed = false;
}

void trackerBridgeOnRx(mesh::Packet* pkt) {
  if (!pkt) return;
  uint8_t hops = pkt->getPathHashCount();
  bool echo = last_tx_len && pkt->getPayloadType() == PAYLOAD_TYPE_GRP_TXT && pkt->payload_len == last_tx_len &&
              memcmp(pkt->payload, last_tx, last_tx_len) == 0;
  if (echo && hops >= 1) {
    echo_hops = hops;
    echo_age_s = last_tx_ms ? (millis() - last_tx_ms) / 1000 : 0;
    if (!last_tx_echoed) {
      last_tx_echoed = true;
      echo_count++;
      scoped_misses = 0;
      scope_warned = false;
      tripLog(EV_ECHO, hops);
      Serial.printf("echo %u hop\n", (unsigned)hops);
    }
    return;
  }
  if (pkt->isRouteFlood() && hops >= 1) flood_heard++;
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
  out->has_fix = have_fix;
  strncpy(out->problem, problem, sizeof(out->problem) - 1);
  out->sent = ever_sent;
  out->lat = sent_lat;
  out->lon = sent_lon;
  out->sequence = sent_seq;
  if (ever_sent) out->age_s = (millis() - sent_ms) / 1000;
  out->no_fix_s = have_fix ? 0 : (millis() - no_fix_since_ms) / 1000;
  out->fixes_sent = fixes_sent;
  out->echoes = echo_count;
  out->echo_hops = echo_hops;
  out->flood_heard = flood_heard;
  NodePrefs* prefs = mesh.getNodePrefs();
  if (prefs->default_scope_name[0]) {
    strncpy(out->scope, prefs->default_scope_name, sizeof(out->scope) - 1);
  }
}
