#pragma once

#include <Arduino.h>
#include <helpers/ContactInfo.h>
#include "TakConfig.h"

struct TakNodeRecord {
  uint8_t pub_key[32];
  char name[32];
  char uid[40];  // meshcore- + 16 hex + NUL
  double lat;
  double lon;
  uint32_t last_heard_ms;
  uint32_t last_sent_ms;
  bool valid;
  bool pending_delete;
};

struct TakRxStats {
  uint32_t packets = 0;      // any MeshCore packet decoded by the radio
  uint32_t adverts = 0;      // adverts heard (with or without position)
  uint32_t adverts_gps = 0;  // adverts that carried a usable position
  uint32_t last_rx_ms = 0;
  uint32_t last_advert_ms = 0;
  uint32_t last_gps_ms = 0;  // last advert that carried a position
  float last_rssi = 0;
  float last_snr = 0;
  char last_advert_name[32] = {0};
  bool last_advert_gps = false;
};

class TakNodes {
public:
  TakRxStats rx;

  void clear();
  // Returns pointer to updated/inserted record, or nullptr if rejected
  TakNodeRecord* upsertFromContact(const ContactInfo& contact, const uint8_t* self_pub);
  TakNodeRecord* findByKey(const uint8_t* pub_key);
  int count() const;
  int sentCount(const TakPrefs& p) const;  // nodes that pass the name filter
  TakNodeRecord* at(int idx);
  const TakNodeRecord* lastHeard() const { return _last_heard; }
  void markSent(TakNodeRecord* n, uint32_t now_ms);
  void markAllForResend();
  void collectRefreshAndExpire(uint32_t now_ms, uint16_t refresh_sec, uint16_t max_age_sec,
                               TakNodeRecord* out_refresh[], int max_refresh, int& n_refresh,
                               TakNodeRecord* out_expire[], int max_expire, int& n_expire);

  static bool hasValidGps(const ContactInfo& c);
  // filter index, -1 = default style, -2 = not sent (matches no filter and send_unmatched is off)
  static int matchFilter(const char* name, const TakPrefs& p);
  static bool passesFilter(const char* name, const TakPrefs& p);
  static const TakCotStyle& styleFor(int match, const TakPrefs& p);
  // name minus the matched start / end text when that filter strips it
  static void callsignFor(const char* name, const TakPrefs& p, char* out, size_t out_len);
  static void makeUid(const uint8_t* pub_key, char* dest, size_t dest_len);

private:
  TakNodeRecord _nodes[TAK_MAX_NODES];
  int _count = 0;
  TakNodeRecord* _last_heard = nullptr;

  int findSlot(const uint8_t* pub_key);
  int oldestSlot();
};
