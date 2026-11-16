#include "TakSeen.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

void TakSeen::clear() {
  memset(_rows, 0, sizeof(_rows));
  _saw_peer = false;
  _superseded = 0;
}

bool TakSeen::isMeshUid(const char* uid) {
  if (!uid || !uid[0]) return false;
  return strncmp(uid, "meshtracker-", 12) == 0 || strncmp(uid, "meshcore-", 9) == 0;
}

uint32_t TakSeen::hashUid(const char* uid) {
  // FNV-1a 32-bit
  uint32_t h = 2166136261u;
  if (!uid) return h;
  for (const unsigned char* p = (const unsigned char*)uid; *p; p++) {
    h ^= *p;
    h *= 16777619u;
  }
  return h ? h : 1u;  // never store 0 as a live hash
}

int64_t TakSeen::parseTimeMs(const char* iso) {
  if (!iso || strlen(iso) < 19) return 0;
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0, ms = 0;
  // YYYY-MM-DDTHH:MM:SS or with fractional seconds
  if (sscanf(iso, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) < 6) return 0;
  const char* dot = strchr(iso, '.');
  if (dot) {
    int frac = 0, n = 0;
    for (const char* p = dot + 1; *p >= '0' && *p <= '9' && n < 3; p++, n++) frac = frac * 10 + (*p - '0');
    while (n < 3) {
      frac *= 10;
      n++;
    }
    ms = frac;
  }
  if (y < 2020 || mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
  // Civil UTC -> unix seconds without relying on the device timezone.
  static const int mdays[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
  int64_t days = (int64_t)(y - 1970) * 365 + (y - 1969) / 4 - (y - 1901) / 100 + (y - 1601) / 400;
  days += mdays[mo - 1] + (d - 1);
  if (mo > 2 && ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0))) days++;
  int64_t sec = days * 86400LL + h * 3600LL + mi * 60LL + s;
  if (sec <= 0) return 0;
  return sec * 1000 + ms;
}

const TakSeenEntry* TakSeen::at(int i) const {
  if (i < 0 || i >= TAK_SEEN_SLOTS) return nullptr;
  return &_rows[i];
}

int TakSeen::find(uint32_t h) const {
  for (int i = 0; i < TAK_SEEN_SLOTS; i++) {
    if (_rows[i].uid_hash == h) return i;
  }
  return -1;
}

int TakSeen::alloc(uint32_t h) {
  int free = -1;
  int oldest = 0;
  for (int i = 0; i < TAK_SEEN_SLOTS; i++) {
    if (_rows[i].uid_hash == 0 && free < 0) free = i;
    if (_rows[i].time_ms < _rows[oldest].time_ms) oldest = i;
  }
  int slot = free >= 0 ? free : oldest;
  _rows[slot].uid_hash = h;
  _rows[slot].time_ms = 0;
  return slot;
}

bool TakSeen::isNewer(const char* uid, int64_t time_ms) const {
  if (!isMeshUid(uid) || time_ms <= 0) return true;
  int i = find(hashUid(uid));
  if (i < 0) return true;
  return time_ms > _rows[i].time_ms;
}

void TakSeen::note(const char* uid, int64_t time_ms) {
  if (!isMeshUid(uid) || time_ms <= 0) return;
  _saw_peer = true;
  uint32_t h = hashUid(uid);
  int i = find(h);
  if (i < 0) i = alloc(h);
  if (time_ms > _rows[i].time_ms) _rows[i].time_ms = time_ms;
}
