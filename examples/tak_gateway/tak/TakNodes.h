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
  time_t heard_utc;  // source time for CoT (advert timestamp or receive time)
  float rssi;
  float snr;
  uint8_t hops;  // 0 = direct
  bool valid;
  bool pending_delete;
  bool needs_send;
  bool superseded;
};

#define TAK_RX_RING 32

struct TakRxSample {
  uint32_t ms;
  float rssi;
  float snr;
  uint8_t hops;
};

struct TakRxStats {
  uint32_t packets = 0;      // any MeshCore packet decoded by the radio
  uint32_t adverts = 0;      // adverts heard (with or without position)
  uint32_t adverts_gps = 0;  // adverts that carried a usable position
  uint32_t flood_hops_unscoped = 0;  // flood packets that already passed through a repeater
  uint32_t flood_hops_scoped = 0;    // same, but carrying a region transport code
  uint16_t last_scoped_code = 0;
  uint32_t last_rx_ms = 0;
  uint32_t last_advert_ms = 0;
  uint32_t last_gps_ms = 0;  // last advert that carried a position
  float last_rssi = 0;
  float last_snr = 0;
  char last_advert_name[32] = {0};
  bool last_advert_gps = false;

  // Direct-vs-relayed ring over recent packets (not limited to GPS adverts).
  TakRxSample ring[TAK_RX_RING] = {};
  uint8_t ring_i = 0;
  uint8_t ring_n = 0;
  float noise_floor = 0;
  bool rx_boost = true;
  uint32_t mt1_ignored = 0;  // !MT1 heard on a channel with parsing off

  void notePacket(float rssi, float snr, uint8_t hops);
  // Best / median RSSI of zero-hop samples in the last window_ms. Returns false if none.
  bool directStats(uint32_t now_ms, uint32_t window_ms, float& best_rssi, float& median_rssi, float& best_snr,
                   uint16_t& direct_n, uint16_t& relayed_n) const;
};

class TakNodes {
public:
  TakRxStats rx;

  void clear();
  // Returns pointer to updated/inserted record, or nullptr if rejected
  TakNodeRecord* upsertFromContact(const ContactInfo& contact, const uint8_t* self_pub, float rssi, float snr,
                                   uint8_t hops, time_t heard_utc);
  TakNodeRecord* findByKey(const uint8_t* pub_key);
  int count() const;
  int sentCount(const TakPrefs& p) const;  // nodes that pass the name filter
  TakNodeRecord* at(int idx);
  const TakNodeRecord* lastHeard() const { return _last_heard; }
  void markSent(TakNodeRecord* n, uint32_t now_ms);
  void markAllForResend();
  // Drop nodes older than max_age_sec from the local table only (no TAK delete).
  void expireLocal(uint32_t now_ms, uint16_t max_age_sec);

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
