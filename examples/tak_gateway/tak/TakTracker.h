#pragma once

// MeshCoreTracker !MT1 parser. No Arduino types: the native unit test compiles this file alone.
// The radio sends identity, role, and the fix. Color, icon, and CoT type stay on the gateway.

#include <stddef.h>
#include <stdint.h>

#define TAK_TRACKER_ID_LEN 9
#define TAK_TRACKER_ROLE_LEN 5
#define TAK_TRACKER_STALE_MIN 5
#define TAK_TRACKER_STALE_MAX 3600
#define TAK_TRACKER_SPEED_MAX_MPS 200.0

enum class TakSeqResult : uint8_t { Accept, Duplicate, Older };

struct TakTrackerMessage {
  char id[TAK_TRACKER_ID_LEN];
  char role[TAK_TRACKER_ROLE_LEN];
  double lat;
  double lon;
  float speed_mps;
  float course_deg;
  float altitude_m;
  uint16_t stale_sec;
  uint32_t sequence;
  int battery_pct;
  bool has_speed;
  bool has_course;
  bool has_altitude;
  bool has_battery;
};

class TakTracker {
public:
  // True when text is the v1 tracker prefix, including a malformed body.
  static bool isTrackerMessage(const char* text);
  static bool parse(const char* text, TakTrackerMessage& out);
  // Unsigned 32-bit sequence. A reboot that jumps forward is accepted; a replay is not.
  static TakSeqResult sequence(bool has_last, uint32_t last, uint32_t q);
};
