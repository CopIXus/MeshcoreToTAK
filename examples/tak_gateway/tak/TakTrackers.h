#pragma once

#include <Arduino.h>
#include "TakConfig.h"
#include "TakTracker.h"

#define TAK_MAX_TRACKERS 24
#define TAK_TRACKER_TIME_SKEW_SEC 300

enum class TakStyleSource : uint8_t { Override, Role, Channel, Default };

struct TakTrackerStats {
  uint32_t heard = 0;
  uint32_t accepted = 0;
  uint32_t duplicates = 0;
  uint32_t out_of_order = 0;
  uint32_t parse_errors = 0;
  uint32_t sent = 0;
};

struct TakTrackerRecord {
  bool valid;
  bool needs_send;
  bool time_fallback;
  char id[TAK_TRACKER_ID_LEN];
  char uid[40];
  char sender[TAK_CALLSIGN_LEN];  // name from the radio, before a gateway override
  char callsign[TAK_CALLSIGN_LEN];
  char role[TAK_TRACKER_ROLE_LEN];
  char style_src[12];
  uint8_t channel;
  double lat;
  double lon;
  float speed_mps;
  float course_deg;
  float altitude_m;
  int battery_pct;
  bool has_speed;
  bool has_course;
  bool has_altitude;
  uint16_t stale_sec;
  uint32_t sequence;
  uint32_t msg_unix;
  time_t cot_time;
  uint32_t last_heard_ms;
  uint32_t last_sent_ms;
  float rssi;
  float snr;
};

class TakClient;

class TakTrackers {
public:
  TakTrackerStats stats;

  void clear();
  // !MT1 on a channel that accepts tracker messages. Never turns the text into chat.
  void ingest(TakClient* client, const TakPrefs& prefs, int channel, const char* sender, const char* text,
              uint32_t msg_unix, float rssi, float snr);
  void markStyleDirty(time_t now);
  void flush(TakClient& client);
  int count() const { return _count; }
  const TakTrackerRecord* at(int idx) const;

  static const char* sourceName(TakStyleSource src);
  static void resolve(const TakPrefs& prefs, int channel, const char* id, const char* role, const char* sender,
                      const TakCotStyle*& style, TakStyleSource& src, char* callsign, size_t callsign_len);

private:
  TakTrackerRecord _rows[TAK_MAX_TRACKERS];
  int _count = 0;

  TakTrackerRecord* find(const char* id);
  TakTrackerRecord* allocate(uint32_t now_ms);
  void trySend(TakClient& client, TakTrackerRecord& rec);
};

extern TakTrackers tak_trackers;
