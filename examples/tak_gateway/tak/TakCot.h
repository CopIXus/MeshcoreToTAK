#pragma once

#include <Arduino.h>
#include "TakConfig.h"
#include "TakNodes.h"

class TakCot {
public:
  // Build one CoT event XML into dest. Returns length, or 0 on failure.
  static size_t buildPoint(char* dest, size_t dest_len, const TakNodeRecord& node,
                           const TakPrefs& prefs, time_t now_utc);

  // Tracker points carry the radio's stale time and a style chosen by the gateway.
  static size_t buildTrackerPoint(char* dest, size_t dest_len, const char* uid, const char* callsign, double lat,
                                  double lon, bool has_alt, float alt_m, bool has_speed, float speed_mps,
                                  bool has_course, float course_deg, const TakCotStyle& style, time_t cot_time,
                                  uint16_t stale_sec);

  static size_t buildDelete(char* dest, size_t dest_len, const char* uid, time_t now_utc);

  // GeoChat (b-t-f) into a named room, same shape RadioTAK / TN SAM use.
  static size_t buildChat(char* dest, size_t dest_len, const char* gw_uid, const char* room,
                          const char* sender, const char* text, float lat, float lon, time_t now_utc);
  static void xmlEscape(const char* in, char* out, size_t out_len);
  static void formatTime(time_t t, char* buf, size_t len);

private:
  static void iconToPath(const char* icon, char* out, size_t out_len);
  static int32_t colorToArgb(const char* hex_color, float opacity);
};
