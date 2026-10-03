#include "TakConfig.h"
#include <string.h>

static_assert(offsetof(TakPrefs, rx_port) == TAK_PREFS_V7_FILE_SIZE, "v7 config file size");

static void copyStr(char* dest, size_t dest_len, const char* src) {
  if (!dest || dest_len == 0) return;
  if (!src) {
    dest[0] = 0;
    return;
  }
  strncpy(dest, src, dest_len - 1);
  dest[dest_len - 1] = 0;
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
  prefs.stale_sec = 180;
  prefs.refresh_sec = 60;
  prefs.max_age_sec = 1800;

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
  copyStr(prefs.node_name, sizeof(prefs.node_name), "TAK-GW");
  prefs.advert_on = false;
  prefs.advert_hours = 12;

  prefs.send_unmatched = true;  // no filters yet: every GPS node uses the default style
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

bool TakConfig::load() {
  setDefaults();
  // Read-link certificate from builds that had TAK -> mesh chat.
  SPIFFS.remove("/tak/rx-client.pem");
  SPIFFS.remove("/tak/rx-client.key");
  if (!SPIFFS.exists("/tak/config.bin")) {
    // Keep the fixed SoftAP password from setDefaults() ("meshcoretak")
    return false;
  }
  File f = SPIFFS.open("/tak/config.bin", "r");
  if (!f) return false;
  TakPrefs tmp;
  memcpy(&tmp, &prefs, sizeof(tmp));  // defaults for any fields the file lacks
  size_t n = f.read((uint8_t*)&tmp, sizeof(tmp));
  f.close();
  bool current = (n == sizeof(tmp) && tmp.version == TAK_CONFIG_VERSION);
  bool v2 = (n == TAK_PREFS_V2_SIZE && tmp.version == 2);
  bool v3 = (n == TAK_PREFS_V3_SIZE && tmp.version == 3);
  bool v4 = (n == TAK_PREFS_V4_SIZE && tmp.version == 4);
  bool v5 = (n == TAK_PREFS_V5_SIZE && tmp.version == 5);
  bool v6 = (n == TAK_PREFS_V6_SIZE && tmp.version == 6);
  bool v7 = (n == TAK_PREFS_V7_FILE_SIZE && tmp.version == 7);
  if (!current && !v2 && !v3 && !v4 && !v5 && !v6 && !v7) {
    return false;
  }
  if (v2) tmp.strip_prefix = false;
  if (v7) tmp.rx_port = tmp.tak_port ? tmp.tak_port : 8089;
  if (!current && !v7) {
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
    tmp.version = TAK_CONFIG_VERSION;
  }
  prefs = tmp;
  if (prefs.ap_password[0] == 0) {
    copyStr(prefs.ap_password, sizeof(prefs.ap_password), "meshcoretak");
  }
  if (prefs.tak_port == 0) prefs.tak_port = 8089;
  if (prefs.rx_port == 0) prefs.rx_port = prefs.tak_port;
  if (prefs.public_room[0] == 0) copyStr(prefs.public_room, sizeof(prefs.public_room), "MeshCore");
  if (strcasecmp(prefs.preset, "CUSTOM") != 0) {
    char name[sizeof(prefs.preset)];
    copyStr(name, sizeof(name), prefs.preset);
    applyPreset(name);
  }
  return true;
}

bool TakConfig::save() {
  prefs.version = TAK_CONFIG_VERSION;
  SPIFFS.mkdir("/tak");
  File f = SPIFFS.open("/tak/config.bin", "w");
  if (!f) return false;
  size_t n = f.write((uint8_t*)&prefs, sizeof(prefs));
  f.close();
  return n == sizeof(prefs);
}

void TakConfig::factoryResetNetworkAndTak() {
  SPIFFS.remove("/tak/config.bin");
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
