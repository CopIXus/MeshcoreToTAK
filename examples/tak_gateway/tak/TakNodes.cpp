#include "TakNodes.h"
#include "TakText.h"
#include <string.h>
#include <ctype.h>

void TakRxStats::notePacket(float rssi, float snr, uint8_t hops) {
  ring[ring_i] = {millis(), rssi, snr, hops};
  ring_i = (uint8_t)((ring_i + 1) % TAK_RX_RING);
  if (ring_n < TAK_RX_RING) ring_n++;
}

bool TakRxStats::directStats(uint32_t now_ms, uint32_t window_ms, float& best_rssi, float& median_rssi,
                             float& best_snr, uint16_t& direct_n, uint16_t& relayed_n) const {
  best_rssi = -200;
  median_rssi = -200;
  best_snr = -100;
  direct_n = 0;
  relayed_n = 0;
  float direct[TAK_RX_RING];
  for (uint8_t i = 0; i < ring_n; i++) {
    const TakRxSample& s = ring[i];
    if (now_ms - s.ms > window_ms) continue;
    if (s.hops == 0) {
      if (s.rssi > best_rssi) best_rssi = s.rssi;
      if (s.snr > best_snr) best_snr = s.snr;
      if (direct_n < TAK_RX_RING) direct[direct_n++] = s.rssi;
    } else {
      relayed_n++;
    }
  }
  if (!direct_n) return false;
  // Insertion sort then pick middle.
  for (uint16_t i = 1; i < direct_n; i++) {
    float v = direct[i];
    int j = (int)i - 1;
    while (j >= 0 && direct[j] > v) {
      direct[j + 1] = direct[j];
      j--;
    }
    direct[j + 1] = v;
  }
  median_rssi = direct[direct_n / 2];
  return true;
}

void TakNodes::clear() {
  memset(_nodes, 0, sizeof(_nodes));
  _count = 0;
  _last_heard = nullptr;
  rx = TakRxStats();
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

int TakNodes::matchFilter(const char* name, const TakPrefs& p) {
  if (name && name[0]) {
    for (int i = 0; i < TAK_MAX_FILTERS; i++) {
      const TakUnitFilter& f = p.filters[i];
      if (!f.enabled || !f.match[0]) continue;
      MatchMode mode = f.mode == 1 ? MatchSuffix : f.mode == 2 ? MatchContains : MatchPrefix;
      if (matchList(f.match, name, mode)) return i;
    }
  }
  return p.send_unmatched ? -1 : -2;
}

bool TakNodes::passesFilter(const char* name, const TakPrefs& p) {
  return matchFilter(name, p) != -2;
}

const TakCotStyle& TakNodes::styleFor(int match, const TakPrefs& p) {
  return match >= 0 && match < TAK_MAX_FILTERS ? p.filters[match].cot : p.cot;
}

void TakNodes::callsignFor(const char* name, const TakPrefs& p, char* out, size_t out_len) {
  if (!out || !out_len) return;
  out[0] = 0;
  if (!name) return;
  size_t nlen = strlen(name), from = 0, to = nlen;
  int m = matchFilter(name, p);
  if (m >= 0 && p.filters[m].strip && p.filters[m].mode < 2) {
    MatchMode mode = p.filters[m].mode == 1 ? MatchSuffix : MatchPrefix;
    size_t l = matchList(p.filters[m].match, name, mode);
    if (mode == MatchPrefix && l < nlen) from = l;
    else if (mode == MatchSuffix && l) to = nlen - l;
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

TakNodeRecord* TakNodes::upsertFromContact(const ContactInfo& contact, const uint8_t* self_pub, float rssi,
                                           float snr, uint8_t hops, time_t heard_utc) {
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
  _nodes[slot].heard_utc = heard_utc;
  _nodes[slot].rssi = rssi;
  _nodes[slot].snr = snr;
  _nodes[slot].hops = hops;
  _nodes[slot].pending_delete = false;
  _nodes[slot].needs_send = true;
  _nodes[slot].superseded = false;
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
  for (int i = 0; i < TAK_MAX_NODES; i++) {
    if (!_nodes[i].valid) continue;
    _nodes[i].last_sent_ms = 0;
    _nodes[i].needs_send = true;
  }
}

void TakNodes::expireLocal(uint32_t now_ms, uint16_t max_age_sec) {
  uint32_t max_age_ms = (uint32_t)max_age_sec * 1000UL;
  for (int i = 0; i < TAK_MAX_NODES; i++) {
    if (!_nodes[i].valid) continue;
    if (now_ms - _nodes[i].last_heard_ms > max_age_ms) {
      _nodes[i].valid = false;
      if (_last_heard == &_nodes[i]) _last_heard = nullptr;
    }
  }
}
