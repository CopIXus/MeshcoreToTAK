#pragma once

#include <Arduino.h>
#include <SPIFFS.h>

#define TAK_CONFIG_VERSION 8
#define TAK_MAX_NODES 32
#define TAK_HOST_LEN 128
#define TAK_PATH_LEN 160
#define TAK_CALLSIGN_LEN 40
#define TAK_ICON_LEN 96
#define TAK_SSID_LEN 64
#define TAK_PSK_LEN 64
#define TAK_PASS_LEN 64
#define TAK_PREFIX_LEN 16
#define TAK_FILTER_LEN 96
#define TAK_CHANNEL_LABEL_LEN 64
#define TAK_PRESET_LEN 16
#define TAK_MAX_CHAT 3
#define TAK_PUBLIC_SLOT TAK_MAX_CHAT  // mesh channel slot used for the Public channel
#define TAK_ROOM_LEN 32
struct TakChatChannel {
  bool enabled;
  char name[32];       // MeshCore channel name ("#hashtag" channels derive their key from it)
  uint8_t secret[32];  // never returned by the web API
  uint8_t secret_len;  // 16 or 32; 0 = no key
  char room[TAK_ROOM_LEN];
};

struct TakCotStyle {
  char type[32];
  char how[16];
  char remarks[64];
  char icon[TAK_ICON_LEN];
  char marker_color[16];
  float marker_opacity;
  bool archived;
};

#define TAK_MAX_FILTERS 8
#define TAK_FILTER_LABEL_LEN 24
// match modes: 0 starts with, 1 ends with, 2 contains
struct TakUnitFilter {
  bool enabled;
  uint8_t mode;
  bool strip;  // drop the matched text from the TAK callsign (starts with / ends with only)
  char label[TAK_FILTER_LABEL_LEN];
  char match[TAK_FILTER_LEN];  // comma-separated, case-insensitive; any entry matches
  TakCotStyle cot;
};

struct TakPrefs {
  uint16_t version;
  bool enabled;

  char wifi_ssid[TAK_SSID_LEN];
  char wifi_psk[TAK_PSK_LEN];
  char setup_password[TAK_PASS_LEN];

  char tak_host[TAK_HOST_LEN];
  uint16_t tak_port;
  char key_passphrase[TAK_PASS_LEN];
  char channel_label[TAK_CHANNEL_LABEL_LEN];

  char preset[TAK_PRESET_LEN];  // US / EU / AU / CUSTOM
  float lora_freq;
  float lora_bw;
  uint8_t lora_sf;
  uint8_t lora_cr;
  int8_t tx_power_dbm;

  bool name_filter;
  char name_prefix[TAK_PREFIX_LEN];  // legacy; migrated into filt_prefix

  uint16_t stale_sec;
  uint16_t refresh_sec;
  uint16_t max_age_sec;

  TakCotStyle cot;

  char ap_password[TAK_PASS_LEN];  // setup SoftAP password
  uint32_t ap_password_seed;

  // ---- v3 (fields below are absent from v2 files; load() defaults them) ----
  bool strip_prefix;  // drop the matched prefix / suffix rule from the TAK callsign

  // ---- v4 ----
  char ui_title[40];
  char banner_text[64];
  char banner_color[8];
  char accent[8];
  bool banner_on;
  bool public_on;  // MeshCore Public channel -> TAK room (listen only)
  char public_room[TAK_ROOM_LEN];

  char chat_callsign[TAK_CALLSIGN_LEN];  // unused since chat became one-way; kept for the file layout
  float chat_lat;
  float chat_lon;
  TakChatChannel chat[TAK_MAX_CHAT];

  // ---- v5 ----
  char node_name[32];     // MeshCore advert name; location comes from chat_lat / chat_lon
  bool advert_on;
  uint16_t advert_hours;  // flood advert interval

  // ---- v6: legacy name filter rules; migrated into filters[] on load ----
  char filt_prefix[TAK_FILTER_LEN];    // replaces name_prefix
  char filt_suffix[TAK_FILTER_LEN];
  char filt_contains[TAK_FILTER_LEN];

  // ---- v7: ordered unit filters, each with its own CoT style ----
  bool send_unmatched;  // nodes matching no filter use cot (the default style)
  TakUnitFilter filters[TAK_MAX_FILTERS];  // first enabled match wins

  // ---- v8: port of the removed read (TAK -> mesh) link; kept for the file layout ----
  uint16_t rx_port;
};

#define TAK_PREFS_V2_SIZE offsetof(TakPrefs, strip_prefix)
#define TAK_PREFS_V4_START offsetof(TakPrefs, ui_title)
#define TAK_PREFS_V5_START offsetof(TakPrefs, node_name)
#define TAK_PREFS_V6_START offsetof(TakPrefs, filt_prefix)
#define TAK_PREFS_V7_START offsetof(TakPrefs, send_unmatched)
// v3 / v4 / v5 files end with the struct's tail padding
#define TAK_PREFS_V3_SIZE ((offsetof(TakPrefs, strip_prefix) + 1 + 3) & ~(size_t)3)
#define TAK_PREFS_V4_SIZE ((TAK_PREFS_V5_START + 3) & ~(size_t)3)
#define TAK_PREFS_V5_SIZE ((TAK_PREFS_V6_START + 3) & ~(size_t)3)
#define TAK_PREFS_V6_SIZE ((TAK_PREFS_V7_START + 3) & ~(size_t)3)
// v7 files are the struct as shipped before rx_port. Confirmed with the ESP32-S3 compiler.
#define TAK_PREFS_V7_FILE_SIZE 4480

class TakConfig {
public:
  TakPrefs prefs;

  void setDefaults();
  bool load();
  bool save();
  void factoryResetNetworkAndTak();

  bool hasWifi() const;
  bool hasTakHost() const;
  bool hasClientCerts() const;

  const char* caPath() const { return "/tak/ca.pem"; }
  const char* certPath() const { return "/tak/client.pem"; }
  const char* keyPath() const { return "/tak/client.key"; }
  const char* logoPath() const { return "/tak/logo.png"; }

  bool writeFile(const char* path, const uint8_t* data, size_t len);
  bool writeFile(const char* path, const String& text);
  String readFile(const char* path) const;

  void applyPreset(const char* name);
};
