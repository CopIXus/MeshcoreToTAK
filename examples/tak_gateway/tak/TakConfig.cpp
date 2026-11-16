#include "TakConfig.h"
#include "TakOtaHealth.h"
#include <string.h>
#include <memory>
#include <new>

static_assert(offsetof(TakPrefs, rx_port) == TAK_PREFS_V7_FILE_SIZE, "v7 config file size");
static_assert(offsetof(TakPrefs, tracker_ch) > offsetof(TakPrefs, rx_port), "v9 tracker config must append");
static_assert(offsetof(TakPrefs, auto_update) == TAK_PREFS_V9_FILE_SIZE, "v10 auto_update must append");

static const char* CFG_PATH = "/tak/config.bin";
static const char* CFG_TMP = "/tak/config.tmp";
static const char* CFG_BAK = "/tak/config.bak";

static void copyStr(char* dest, size_t dest_len, const char* src) {
  if (!dest || dest_len == 0) return;
  if (!src) {
    dest[0] = 0;
    return;
  }
  strncpy(dest, src, dest_len - 1);
  dest[dest_len - 1] = 0;
}

void TakConfig::ensureUniqueNodeName() {
  if (prefs.node_name[0] && strcmp(prefs.node_name, "TAK-GW") != 0) return;
  uint16_t suffix = (uint16_t)(ESP.getEfuseMac() & 0xFFFFULL);
  snprintf(prefs.node_name, sizeof(prefs.node_name), "TAK-GW-%04X", (unsigned)suffix);
}

void TakConfig::setDefaults() {
  memset(&prefs, 0, sizeof(prefs));
  prefs.version = TAK_CONFIG_VERSION;
  prefs.enabled = false;
  prefs.tak_port = 8089;
  prefs.rx_port = 8089;
  copyStr(prefs.preset, sizeof(prefs.preset), "US");
  applyPreset("US");
  prefs.name_filter = false;
  copyStr(prefs.name_prefix, sizeof(prefs.name_prefix), "tak_");
  copyStr(prefs.filt_prefix, sizeof(prefs.filt_prefix), "tak_");
  prefs.stale_sec = 600;
  prefs.refresh_sec = 60;
  prefs.max_age_sec = 1800;
  prefs.auto_update = TAK_AUTO_UPDATE_AUTO;

  copyStr(prefs.cot.type, sizeof(prefs.cot.type), "a-f-G-U-U-S-R");
  copyStr(prefs.cot.how, sizeof(prefs.cot.how), "h-g-i-g-o");
  copyStr(prefs.cot.remarks, sizeof(prefs.cot.remarks), "MeshCore node");
  prefs.cot.icon[0] = 0;  // no usericon: the 2525 symbol comes from cot.type
  copyStr(prefs.cot.marker_color, sizeof(prefs.cot.marker_color), "#0010eb");
  prefs.cot.marker_opacity = 1.0f;
  prefs.cot.archived = false;

  copyStr(prefs.setup_password, sizeof(prefs.setup_password), "meshcore");
  copyStr(prefs.key_passphrase, sizeof(prefs.key_passphrase), "atakatak");
  // SoftAP password: fixed field default for easy setup; shown on OLED
  copyStr(prefs.ap_password, sizeof(prefs.ap_password), "meshcoretak");

  copyStr(prefs.ui_title, sizeof(prefs.ui_title), "MeshCore TAK Gateway");
  copyStr(prefs.banner_color, sizeof(prefs.banner_color), "#f1f5f9");
  copyStr(prefs.accent, sizeof(prefs.accent), "#3dd68c");
  copyStr(prefs.chat_callsign, sizeof(prefs.chat_callsign), "MeshCore GW");
  copyStr(prefs.public_room, sizeof(prefs.public_room), "MeshCore");
  ensureUniqueNodeName();
  prefs.advert_on = false;
  prefs.advert_hours = 12;

  prefs.send_unmatched = true;  // no filters yet: every GPS node uses the default style

  // Tracker parsing is off. The role catalog is ready so the first k= fix has a style.
  auto seedRole = [&](int i, const char* key, const char* type, const char* remarks, const char* color) {
    TakRoleStyle& r = prefs.roles[i];
    r.enabled = true;
    copyStr(r.key, sizeof(r.key), key);
    copyStr(r.cot.type, sizeof(r.cot.type), type);
    copyStr(r.cot.how, sizeof(r.cot.how), "m-g");
    copyStr(r.cot.remarks, sizeof(r.cot.remarks), remarks);
    copyStr(r.cot.marker_color, sizeof(r.cot.marker_color), color);
    r.cot.marker_opacity = 1.0f;
  };
  seedRole(0, "k9", "a-f-G-U-U-L", "K9", "#0010EB");
  seedRole(1, "veh", "a-f-G-E-V", "Vehicle", "#F59E0B");
  seedRole(2, "per", "a-f-G-U-C-I", "Person", "#22C55E");
  seedRole(3, "fw", "a-f-G-E-V", "Fire", "#EF4444");
  seedRole(4, "ems", "a-f-G-E-V-U-A", "EMS", "#F97316");
  seedRole(5, "cmd", "a-f-G-U-H", "Command", "#A855F7");
  for (int i = 0; i < TAK_MAX_CHAT; i++) {
    TakChannelTracker& t = prefs.tracker_ch[i];
    t.enabled = false;
    t.cot = prefs.cot;
    copyStr(t.cot.how, sizeof(t.cot.how), "m-g");
    copyStr(t.cot.remarks, sizeof(t.cot.remarks), "MeshCore tracker");
  }
}

void TakConfig::applyPreset(const char* name) {
  if (!name) name = "US";
  copyStr(prefs.preset, sizeof(prefs.preset), name);

  // MeshCore "narrow" regional presets (BW 62.5). Nodes on a different BW/SF cannot be heard at all.
  if (strcasecmp(name, "EU") == 0) {
    prefs.lora_freq = 869.618f;
    prefs.lora_bw = 62.5f;
    prefs.lora_sf = 8;
    prefs.lora_cr = 5;
  } else if (strcasecmp(name, "AU") == 0) {
    prefs.lora_freq = 916.8f;
    prefs.lora_bw = 250.0f;
    prefs.lora_sf = 11;
    prefs.lora_cr = 5;
  } else if (strcasecmp(name, "CUSTOM") == 0) {
    // leave numeric fields as-is
  } else {
    // US / default
    copyStr(prefs.preset, sizeof(prefs.preset), "US");
    prefs.lora_freq = 910.525f;
    prefs.lora_bw = 62.5f;
    prefs.lora_sf = 7;
    prefs.lora_cr = 5;
  }
  if (prefs.tx_power_dbm == 0) prefs.tx_power_dbm = 22;
}

bool TakConfig::readPrefsFile(const char* path, TakPrefs& out, size_t& n_out) {
  n_out = 0;
  if (!SPIFFS.exists(path)) return false;
  File f = SPIFFS.open(path, "r");
  if (!f) return false;
  n_out = f.read((uint8_t*)&out, sizeof(out));
  f.close();
  return n_out > 0;
}

static bool acceptVersion(size_t n, uint16_t ver, bool& current, bool& v2, bool& v3, bool& v4, bool& v5, bool& v6,
                          bool& v7, bool& v8, bool& v9) {
  current = (n == sizeof(TakPrefs) && ver == TAK_CONFIG_VERSION);
  v2 = (n == TAK_PREFS_V2_SIZE && ver == 2);
  v3 = (n == TAK_PREFS_V3_SIZE && ver == 3);
  v4 = (n == TAK_PREFS_V4_SIZE && ver == 4);
  v5 = (n == TAK_PREFS_V5_SIZE && ver == 5);
  v6 = (n == TAK_PREFS_V6_SIZE && ver == 6);
  v7 = (n == TAK_PREFS_V7_FILE_SIZE && ver == 7);
  v8 = (n == TAK_PREFS_V8_FILE_SIZE && ver == 8);
  v9 = (n == TAK_PREFS_V9_FILE_SIZE && ver == 9);
  // Also accept a truncated current-size write that still carries version 10.
  if (!current && n >= TAK_PREFS_V9_FILE_SIZE && ver == TAK_CONFIG_VERSION && n <= sizeof(TakPrefs)) {
    current = (n == sizeof(TakPrefs));
    if (!current && n == TAK_PREFS_V9_FILE_SIZE) v9 = true;
  }
  return current || v2 || v3 || v4 || v5 || v6 || v7 || v8 || v9;
}

bool TakConfig::load() {
  _load_failed = false;
  setDefaults();
  // Read-link certificate from builds that had TAK -> mesh chat.
  for (const char* old : {"/tak/rx-client.pem", "/tak/rx-client.key"}) {
    if (SPIFFS.exists(old)) SPIFFS.remove(old);
  }
  if (!SPIFFS.exists(CFG_PATH) && !SPIFFS.exists(CFG_BAK)) {
    return false;
  }

  std::unique_ptr<TakPrefs> holder(new (std::nothrow) TakPrefs);
  if (!holder) {
    _load_failed = SPIFFS.exists(CFG_PATH);
    return false;
  }
  TakPrefs& tmp = *holder;
  memcpy(&tmp, &prefs, sizeof(tmp));

  size_t n = 0;
  bool from_backup = false;
  if (!readPrefsFile(CFG_PATH, tmp, n)) {
    memcpy(&tmp, &prefs, sizeof(tmp));
    if (!readPrefsFile(CFG_BAK, tmp, n)) {
      _load_failed = SPIFFS.exists(CFG_PATH) || SPIFFS.exists(CFG_BAK);
      return false;
    }
    from_backup = true;
    Serial.println("[CFG] primary config unreadable, using backup");
  }

  bool current = false, v2 = false, v3 = false, v4 = false, v5 = false, v6 = false, v7 = false, v8 = false,
       v9 = false;
  if (!acceptVersion(n, tmp.version, current, v2, v3, v4, v5, v6, v7, v8, v9)) {
    // Try backup if primary looked corrupt.
    if (!from_backup && SPIFFS.exists(CFG_BAK)) {
      memcpy(&tmp, &prefs, sizeof(tmp));
      if (readPrefsFile(CFG_BAK, tmp, n) &&
          acceptVersion(n, tmp.version, current, v2, v3, v4, v5, v6, v7, v8, v9)) {
        from_backup = true;
        Serial.println("[CFG] primary config corrupt, using backup");
      } else {
        _load_failed = true;
        Serial.println("[CFG] config unreadable - leaving file alone");
        return false;
      }
    } else {
      _load_failed = true;
      Serial.println("[CFG] config unreadable - leaving file alone");
      return false;
    }
  }

  if (v2) tmp.strip_prefix = false;
  if (v7) tmp.rx_port = tmp.tak_port ? tmp.tak_port : 8089;
  if (v8 || v9) {
    // tracker / auto_update fields stay at the defaults copied above for missing tails
  }
  if (!current && !v7 && !v8 && !v9) {
    // default everything the old file lacks (its tail padding overlapped the first new bytes)
    size_t from = v6 ? TAK_PREFS_V7_START : v5 ? TAK_PREFS_V6_START : v4 ? TAK_PREFS_V5_START : TAK_PREFS_V4_START;
    memcpy((uint8_t*)&tmp + from, (const uint8_t*)&prefs + from, sizeof(tmp) - from);
    tmp.name_prefix[sizeof(tmp.name_prefix) - 1] = 0;
    if (!v6) copyStr(tmp.filt_prefix, sizeof(tmp.filt_prefix), tmp.name_prefix);
    // turn the old rule lists into ordered filters that keep the old style
    tmp.send_unmatched = !tmp.name_filter;
    const char* lists[3] = {tmp.filt_prefix, tmp.filt_suffix, tmp.filt_contains};
    const char* labels[3] = {"Starts with", "Ends with", "Contains"};
    for (int i = 0; i < 3; i++) {
      if (!lists[i][0]) continue;
      TakUnitFilter& f = tmp.filters[i];
      f.enabled = true;
      f.mode = (uint8_t)i;
      f.strip = tmp.strip_prefix;
      copyStr(f.label, sizeof(f.label), labels[i]);
      copyStr(f.match, sizeof(f.match), lists[i]);
      f.cot = tmp.cot;
    }
  }
  if (!current) {
    if (tmp.stale_sec < 600) tmp.stale_sec = 600;  // v10 one-shot raise
    if (tmp.auto_update > TAK_AUTO_UPDATE_AUTO) tmp.auto_update = TAK_AUTO_UPDATE_AUTO;
    if (v8 || v9 || v7 || v6 || v5 || v4 || v3 || v2) {
      // auto_update default already set from setDefaults memcpy above when missing
      if (v9 || v8 || v7 || v6 || v5 || v4 || v3 || v2) {
        if (n < offsetof(TakPrefs, auto_update) + sizeof(tmp.auto_update)) {
          tmp.auto_update = TAK_AUTO_UPDATE_AUTO;
        }
      }
    }
    tmp.version = TAK_CONFIG_VERSION;
  }
  prefs = tmp;
  if (prefs.ap_password[0] == 0) {
    copyStr(prefs.ap_password, sizeof(prefs.ap_password), "meshcoretak");
  }
  if (prefs.tak_port == 0) prefs.tak_port = 8089;
  if (prefs.rx_port == 0) prefs.rx_port = prefs.tak_port;
  if (prefs.public_room[0] == 0) copyStr(prefs.public_room, sizeof(prefs.public_room), "MeshCore");
  if (prefs.auto_update > TAK_AUTO_UPDATE_AUTO) prefs.auto_update = TAK_AUTO_UPDATE_AUTO;
  ensureUniqueNodeName();
  if (strcasecmp(prefs.preset, "CUSTOM") != 0) {
    char name[sizeof(prefs.preset)];
    copyStr(name, sizeof(name), prefs.preset);
    applyPreset(name);
  }
  return true;
}

bool TakConfig::writePrefsBytes(const uint8_t* data, size_t len) {
  SPIFFS.mkdir("/tak");
  SPIFFS.remove(CFG_TMP);
  File f = SPIFFS.open(CFG_TMP, "w");
  if (!f) return false;
  size_t n = f.write(data, len);
  f.flush();
  f.close();
  if (n != len) {
    SPIFFS.remove(CFG_TMP);
    return false;
  }
  // Keep the previous file as backup, then promote the temp.
  if (SPIFFS.exists(CFG_PATH)) {
    SPIFFS.remove(CFG_BAK);
    if (!SPIFFS.rename(CFG_PATH, CFG_BAK)) {
      // rename failed: try copy-fallback so we still do not truncate the live file first
      File src = SPIFFS.open(CFG_PATH, "r");
      File dst = SPIFFS.open(CFG_BAK, "w");
      if (src && dst) {
        uint8_t buf[256];
        while (src.available()) {
          int r = src.read(buf, sizeof(buf));
          if (r > 0) dst.write(buf, r);
        }
      }
      if (src) src.close();
      if (dst) dst.close();
      SPIFFS.remove(CFG_PATH);
    }
  }
  if (!SPIFFS.rename(CFG_TMP, CFG_PATH)) {
    // Last resort: write directly.
    File out = SPIFFS.open(CFG_PATH, "w");
    if (!out) return false;
    File in = SPIFFS.open(CFG_TMP, "r");
    if (!in) {
      out.close();
      return false;
    }
    uint8_t buf[256];
    size_t wrote = 0;
    while (in.available()) {
      int r = in.read(buf, sizeof(buf));
      if (r <= 0) break;
      wrote += out.write(buf, r);
    }
    in.close();
    out.close();
    SPIFFS.remove(CFG_TMP);
    return wrote == len;
  }
  return true;
}

bool TakConfig::save() {
  if (_load_failed) {
    Serial.println("[CFG] refusing to overwrite unreadable config");
    return false;
  }
  // While an OTA image is pending verify, keep the on-disk layout at v9 so a
  // rollback can still read the file.
  bool write_v10 = TakOtaHealth::mayWriteV10();
  prefs.version = write_v10 ? TAK_CONFIG_VERSION : 9;
  size_t len = write_v10 ? sizeof(prefs) : TAK_PREFS_V9_FILE_SIZE;
  bool ok = writePrefsBytes((const uint8_t*)&prefs, len);
  prefs.version = TAK_CONFIG_VERSION;  // keep RAM at current schema either way
  return ok;
}

void TakConfig::factoryResetNetworkAndTak() {
  _load_failed = false;
  SPIFFS.remove(CFG_PATH);
  SPIFFS.remove(CFG_TMP);
  SPIFFS.remove(CFG_BAK);
  SPIFFS.remove(caPath());
  SPIFFS.remove(certPath());
  SPIFFS.remove(keyPath());
  SPIFFS.remove(logoPath());
  setDefaults();
  save();
}

bool TakConfig::hasWifi() const {
  return prefs.wifi_ssid[0] != 0;
}

bool TakConfig::hasTakHost() const {
  return prefs.tak_host[0] != 0;
}

bool TakConfig::hasClientCerts() const {
  return SPIFFS.exists(caPath()) && SPIFFS.exists(certPath()) && SPIFFS.exists(keyPath());
}

bool TakConfig::writeFile(const char* path, const uint8_t* data, size_t len) {
  SPIFFS.mkdir("/tak");
  File f = SPIFFS.open(path, "w");
  if (!f) return false;
  size_t n = f.write(data, len);
  f.close();
  return n == len;
}

bool TakConfig::writeFile(const char* path, const String& text) {
  return writeFile(path, (const uint8_t*)text.c_str(), text.length());
}

String TakConfig::readFile(const char* path) const {
  if (!SPIFFS.exists(path)) return String();
  File f = SPIFFS.open(path, "r");
  if (!f) return String();
  String s = f.readString();
  f.close();
  return s;
}
