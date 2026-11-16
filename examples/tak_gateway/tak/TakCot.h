#pragma once

#include <Arduino.h>
#include "TakConfig.h"
#include "TakNodes.h"

class TakCot {
public:
  // Build one CoT event XML into dest. Returns length, or 0 on failure.
  // cot_time is when the radio was last seen (source time), not when this gateway sends.
  static size_t buildPoint(char* dest, size_t dest_len, const TakNodeRecord& node, const TakPrefs& prefs,
                           time_t cot_time);

  // Tracker points use the gateway stale setting and a style chosen by the gateway.
  static size_t buildTrackerPoint(char* dest, size_t dest_len, const char* uid, const char* callsign, double lat,
                                  double lon, bool has_alt, float alt_m, bool has_speed, float speed_mps,
                                  bool has_course, float course_deg, const TakCotStyle& style, time_t cot_time,
                                  uint16_t stale_sec, const char* gateway, const char* group);

  // "role | gateway | seen 16 Nov 14:32:05Z"
  static void formatRemarks(const char* role, const char* gateway, time_t seen_utc, char* out, size_t out_len);

  static size_t buildDelete(char* dest, size_t dest_len, const char* uid, time_t now_utc, const char* group);

  // GeoChat (b-t-f) into a named room, same shape RadioTAK / TN SAM use.
  // group is the TAK Server channel (__group). An empty group is filed as __ANON__.
  static size_t buildChat(char* dest, size_t dest_len, const char* gw_uid, const char* room,
                          const char* sender, const char* text, float lat, float lon, time_t now_utc,
                          const char* group);
  static void xmlEscape(const char* in, char* out, size_t out_len);
  static void formatTime(time_t t, char* buf, size_t len);

private:
  static void iconToPath(const char* icon, char* out, size_t out_len);
  static int32_t colorToArgb(const char* hex_color, float opacity);
};
