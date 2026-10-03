#include "TakTracker.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <errno.h>

bool TakTracker::isTrackerMessage(const char* text) {
  if (!text || strncmp(text, "!MT1", 4) != 0) return false;
  char next = text[4];
  return next == 0 || next == ';' || next == ' ' || next == '\r' || next == '\n';
}

static bool keyIs(const char* s, size_t n, const char* lit) {
  size_t L = strlen(lit);
  return n == L && memcmp(s, lit, L) == 0;
}

static bool copyValue(const char* s, size_t n, char* buf, size_t buf_len) {
  if (n == 0 || n >= buf_len) return false;
  memcpy(buf, s, n);
  buf[n] = 0;
  return true;
}

static bool parseDouble(const char* s, size_t n, double& out) {
  char buf[32];
  if (!copyValue(s, n, buf, sizeof(buf))) return false;
  char* end = nullptr;
  errno = 0;
  double v = strtod(buf, &end);
  if (errno || !end || end == buf || *end != 0 || !std::isfinite(v)) return false;
  out = v;
  return true;
}

static bool parseU32(const char* s, size_t n, uint32_t& out) {
  char buf[16];
  if (!copyValue(s, n, buf, sizeof(buf))) return false;
  if (buf[0] == '-' || buf[0] == '+') return false;
  char* end = nullptr;
  errno = 0;
  unsigned long v = strtoul(buf, &end, 10);
  if (errno || !end || end == buf || *end != 0 || v > 0xFFFFFFFFUL) return false;
  out = (uint32_t)v;
  return true;
}

static bool parseI32(const char* s, size_t n, long& out) {
  char buf[16];
  if (!copyValue(s, n, buf, sizeof(buf))) return false;
  if (buf[0] == '+') return false;
  char* end = nullptr;
  errno = 0;
  long v = strtol(buf, &end, 10);
  if (errno || !end || end == buf || *end != 0) return false;
  out = v;
  return true;
}

bool TakTracker::parse(const char* text, TakTrackerMessage& out) {
  memset(&out, 0, sizeof(out));
  out.battery_pct = -1;
  if (!text || strncmp(text, "!MT1;", 5) != 0) return false;

  const unsigned F_U = 1u << 0;
  const unsigned F_K = 1u << 1;
  const unsigned F_LA = 1u << 2;
  const unsigned F_LN = 1u << 3;
  const unsigned F_S = 1u << 4;
  const unsigned F_C = 1u << 5;
  const unsigned F_A = 1u << 6;
  const unsigned F_ST = 1u << 7;
  const unsigned F_Q = 1u << 8;
  const unsigned F_B = 1u << 9;
  unsigned seen = 0;

  const char* p = text + 5;
  while (*p) {
    const char* semi = strchr(p, ';');
    size_t len = semi ? (size_t)(semi - p) : strlen(p);
    if (len == 0) {
      if (!semi) break;
      p = semi + 1;
      continue;
    }
    const void* eqp = memchr(p, '=', len);
    if (!eqp) return false;
    const char* eq = (const char*)eqp;
    size_t klen = (size_t)(eq - p);
    const char* val = eq + 1;
    size_t vlen = len - klen - 1;
    if (klen == 0) return false;

    auto take = [&](unsigned bit) -> bool {
      if (seen & bit) return false;
      seen |= bit;
      return true;
    };

    if (keyIs(p, klen, "u")) {
      if (!take(F_U) || vlen != 8) return false;
      for (size_t i = 0; i < 8; i++) {
        char c = val[i];
        bool hex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
        if (!hex) return false;
        if (c >= 'a' && c <= 'f') c = (char)(c - 'a' + 'A');
        out.id[i] = c;
      }
      out.id[8] = 0;
    } else if (keyIs(p, klen, "k")) {
      if (!take(F_K) || vlen < 2 || vlen > 4) return false;
      for (size_t i = 0; i < vlen; i++) {
        char c = val[i];
        bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        if (!ok) return false;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        out.role[i] = c;
      }
      out.role[vlen] = 0;
    } else if (keyIs(p, klen, "la")) {
      if (!take(F_LA)) return false;
      double v = 0;
      if (!parseDouble(val, vlen, v) || v < -90.0 || v > 90.0) return false;
      out.lat = v;
    } else if (keyIs(p, klen, "ln")) {
      if (!take(F_LN)) return false;
      double v = 0;
      if (!parseDouble(val, vlen, v) || v < -180.0 || v > 180.0) return false;
      out.lon = v;
    } else if (keyIs(p, klen, "s")) {
      if (!take(F_S)) return false;
      double v = 0;
      if (!parseDouble(val, vlen, v) || v < 0.0 || v > TAK_TRACKER_SPEED_MAX_MPS) return false;
      out.speed_mps = (float)v;
      out.has_speed = true;
    } else if (keyIs(p, klen, "c")) {
      if (!take(F_C)) return false;
      double v = 0;
      if (!parseDouble(val, vlen, v) || v < 0.0 || v >= 360.0) return false;
      out.course_deg = (float)v;
      out.has_course = true;
    } else if (keyIs(p, klen, "a")) {
      if (!take(F_A)) return false;
      double v = 0;
      if (!parseDouble(val, vlen, v) || v < -1000.0 || v > 20000.0) return false;
      out.altitude_m = (float)v;
      out.has_altitude = true;
    } else if (keyIs(p, klen, "st")) {
      if (!take(F_ST)) return false;
      uint32_t v = 0;
      if (!parseU32(val, vlen, v) || v < TAK_TRACKER_STALE_MIN || v > TAK_TRACKER_STALE_MAX) return false;
      out.stale_sec = (uint16_t)v;
    } else if (keyIs(p, klen, "q")) {
      if (!take(F_Q)) return false;
      if (!parseU32(val, vlen, out.sequence)) return false;
    } else if (keyIs(p, klen, "b")) {
      if (!take(F_B)) return false;
      long v = 0;
      if (!parseI32(val, vlen, v) || v < 0 || v > 100) return false;
      out.battery_pct = (int)v;
      out.has_battery = true;
    }
    // Unknown keys are ignored so a later protocol field does not break this gateway.

    if (!semi) break;
    p = semi + 1;
  }

  const unsigned need = F_U | F_K | F_LA | F_LN | F_ST | F_Q;
  if ((seen & need) != need) return false;
  if (out.lat == 0.0 && out.lon == 0.0) return false;
  return true;
}

TakSeqResult TakTracker::sequence(bool has_last, uint32_t last, uint32_t q) {
  if (!has_last) return TakSeqResult::Accept;
  int32_t delta = (int32_t)(q - last);
  if (delta == 0) return TakSeqResult::Duplicate;
  if (delta > 0) return TakSeqResult::Accept;
  return TakSeqResult::Older;
}
