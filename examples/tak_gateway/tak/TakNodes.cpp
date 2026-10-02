#include "TakNodes.h"
#include "TakText.h"
#include <string.h>
#include <ctype.h>

void TakNodes::clear() {
  memset(_nodes, 0, sizeof(_nodes));
  _count = 0;
  _last_heard = nullptr;
}

bool TakNodes::hasValidGps(const ContactInfo& c) {
  if (c.gps_lat == 0 && c.gps_lon == 0) return false;
  // µdegrees: reject nonsense
  if (c.gps_lat < -90 * 1000000L || c.gps_lat > 90 * 1000000L) return false;
  if (c.gps_lon < -180 * 1000000L || c.gps_lon > 180 * 1000000L) return false;
  return true;
}

static bool ieq(const char* a, const char* b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
  }
  return true;
}

enum MatchMode { MatchPrefix, MatchSuffix, MatchContains };

// Length of the longest token in a comma-separated list that matches name; 0 if none.
static size_t matchList(const char* list, const char* name, MatchMode mode) {
  size_t nlen = strlen(name), best = 0;
  const char* s = list;
  while (s && *s) {
    const char* e = strchr(s, ',');
    if (!e) e = s + strlen(s);
    const char* a = s;
    const char* b = e;
    while (a < b && isspace((unsigned char)*a)) a++;
    while (b > a && isspace((unsigned char)b[-1])) b--;
    size_t t = b - a;
    if (t && t <= nlen && t > best) {
      bool hit = false;
      if (mode == MatchPrefix) {
        hit = ieq(name, a, t);
      } else if (mode == MatchSuffix) {
        hit = ieq(name + nlen - t, a, t);
      } else {
        for (size_t i = 0; i + t <= nlen && !hit; i++) hit = ieq(name + i, a, t);
      }
      if (hit) best = t;
    }
    s = *e ? e + 1 : e;
  }
  return best;
}

bool TakNodes::passesFilter(const char* name, const TakPrefs& p) {
  if (!p.name_filter) return true;
  if (!name || !name[0]) return false;
  return matchList(p.filt_prefix, name, MatchPrefix) || matchList(p.filt_suffix, name, MatchSuffix) ||
         matchList(p.filt_contains, name, MatchContains);
}

void TakNodes::callsignFor(const char* name, const TakPrefs& p, char* out, size_t out_len) {
  if (!out || !out_len) return;
  out[0] = 0;
  if (!name) return;
  size_t nlen = strlen(name), from = 0, to = nlen;
  if (p.strip_prefix) {
    size_t pl = matchList(p.filt_prefix, name, MatchPrefix);
    size_t sl = matchList(p.filt_suffix, name, MatchSuffix);
    if (pl < nlen) from = pl;
    if (sl && nlen - sl > from) to = nlen - sl;
    while (from < to && isspace((unsigned char)name[from])) from++;
    while (to > from && isspace((unsigned char)name[to - 1])) to--;
    if (from >= to) from = 0, to = nlen;  // never strip a name down to nothing
  }
  size_t n = to - from < out_len - 1 ? to - from : out_len - 1;
  memcpy(out, name + from, n);
  out[n] = 0;
}

int TakNodes::sentCount(const TakPrefs& p) const {
  int n = 0;
  for (int i = 0; i < TAK_MAX_NODES; i++) {
    if (_nodes[i].valid && passesFilter(_nodes[i].name, p)) n++;
  }
  return n;
}

void TakNodes::makeUid(const uint8_t* pub_key, char* dest, size_t dest_len) {
  static const char* hex = "0123456789abcdef";
  // meshcore- + 16 hex chars
  if (dest_len < 10 + 16 + 1) {
    dest[0] = 0;
    return;
  }
  memcpy(dest, "meshcore-", 9);
  for (int i = 0; i < 8; i++) {
    dest[9 + i * 2] = hex[(pub_key[i] >> 4) & 0xF];
    dest[9 + i * 2 + 1] = hex[pub_key[i] & 0xF];
  }
  dest[9 + 16] = 0;
}

int TakNodes::findSlot(const uint8_t* pub_key) {
  for (int i = 0; i < TAK_MAX_NODES; i++) {
    if (_nodes[i].valid && memcmp(_nodes[i].pub_key, pub_key, 32) == 0) return i;
  }
  return -1;
}

int TakNodes::oldestSlot() {
  int best = -1;
  uint32_t oldest = UINT32_MAX;
  for (int i = 0; i < TAK_MAX_NODES; i++) {
    if (!_nodes[i].valid) return i;
    if (_nodes[i].last_heard_ms < oldest) {
      oldest = _nodes[i].last_heard_ms;
      best = i;
    }
  }
  return best;
}

TakNodeRecord* TakNodes::upsertFromContact(const ContactInfo& contact, const uint8_t* self_pub) {
  if (self_pub && memcmp(contact.id.pub_key, self_pub, 32) == 0) {
    return nullptr;  // never track self
  }
  if (!hasValidGps(contact)) return nullptr;

  int slot = findSlot(contact.id.pub_key);
  if (slot < 0) {
    slot = oldestSlot();
    if (slot < 0) return nullptr;
    memset(&_nodes[slot], 0, sizeof(_nodes[slot]));
    memcpy(_nodes[slot].pub_key, contact.id.pub_key, 32);
    makeUid(contact.id.pub_key, _nodes[slot].uid, sizeof(_nodes[slot].uid));
    _nodes[slot].valid = true;
    if (_count < TAK_MAX_NODES) _count++;
  }

  utf8Copy(_nodes[slot].name, sizeof(_nodes[slot].name), contact.name);
  _nodes[slot].lat = ((double)contact.gps_lat) / 1000000.0;
  _nodes[slot].lon = ((double)contact.gps_lon) / 1000000.0;
  _nodes[slot].last_heard_ms = millis();
  _nodes[slot].pending_delete = false;
  _last_heard = &_nodes[slot];
  return &_nodes[slot];  // recorded even if the name filter rejects it; callers check passesFilter()
}

TakNodeRecord* TakNodes::findByKey(const uint8_t* pub_key) {
  int s = findSlot(pub_key);
  return s < 0 ? nullptr : &_nodes[s];
}

int TakNodes::count() const {
  int n = 0;
  for (int i = 0; i < TAK_MAX_NODES; i++) if (_nodes[i].valid) n++;
  return n;
}

TakNodeRecord* TakNodes::at(int idx) {
  int n = 0;
  for (int i = 0; i < TAK_MAX_NODES; i++) {
    if (!_nodes[i].valid) continue;
    if (n == idx) return &_nodes[i];
    n++;
  }
  return nullptr;
}

void TakNodes::markSent(TakNodeRecord* rec, uint32_t now_ms) {
  if (rec) rec->last_sent_ms = now_ms;
}

void TakNodes::markAllForResend() {
  for (int i = 0; i < TAK_MAX_NODES; i++) _nodes[i].last_sent_ms = 0;
}

void TakNodes::collectRefreshAndExpire(uint32_t now_ms, uint16_t refresh_sec, uint16_t max_age_sec,
                                       TakNodeRecord* out_refresh[], int max_refresh, int& n_refresh,
                                       TakNodeRecord* out_expire[], int max_expire, int& n_expire) {
  n_refresh = 0;
  n_expire = 0;
  uint32_t refresh_ms = (uint32_t)refresh_sec * 1000UL;
  uint32_t max_age_ms = (uint32_t)max_age_sec * 1000UL;

  for (int i = 0; i < TAK_MAX_NODES; i++) {
    if (!_nodes[i].valid) continue;
    uint32_t age = now_ms - _nodes[i].last_heard_ms;
    if (age > max_age_ms) {
      if (n_expire < max_expire) out_expire[n_expire++] = &_nodes[i];
      continue;
    }
    if (_nodes[i].last_sent_ms == 0 || (now_ms - _nodes[i].last_sent_ms) >= refresh_ms) {
      if (n_refresh < max_refresh) out_refresh[n_refresh++] = &_nodes[i];
    }
  }
}
