#pragma once

// Newest time seen on the TAK server for MeshCore marker UIDs. No Arduino types:
// the native unit test compiles this file alone.

#include <stddef.h>
#include <stdint.h>

#define TAK_SEEN_SLOTS 48

struct TakSeenEntry {
  uint32_t uid_hash;
  int64_t time_ms;  // CoT sendTime / event time as unix milliseconds (0 = empty)
};

class TakSeen {
public:
  void clear();
  // True when this uid's time is newer than anything recorded for it.
  bool isNewer(const char* uid, int64_t time_ms) const;
  // Record an inbound (or successfully sent) event. Ignores empty / non-mesh UIDs.
  void note(const char* uid, int64_t time_ms);
  // True once any meshcore-/meshtracker- uid has been observed from the server.
  bool sawPeer() const { return _saw_peer; }
  uint32_t superseded() const { return _superseded; }
  void countSuperseded() { _superseded++; }

  static bool isMeshUid(const char* uid);
  static uint32_t hashUid(const char* uid);
  // Parse CoT ISO time "YYYY-MM-DDTHH:MM:SS[.fff]Z" to unix milliseconds. 0 on failure.
  static int64_t parseTimeMs(const char* iso);
  static int64_t timeSecToMs(int64_t sec) { return sec > 0 ? sec * 1000 : 0; }

  // Exposed for tests.
  const TakSeenEntry* at(int i) const;
  int capacity() const { return TAK_SEEN_SLOTS; }

private:
  TakSeenEntry _rows[TAK_SEEN_SLOTS] = {};
  bool _saw_peer = false;
  uint32_t _superseded = 0;
  int find(uint32_t h) const;
  int alloc(uint32_t h);
};
