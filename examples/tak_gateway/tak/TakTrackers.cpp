#include "TakTrackers.h"

#include "TakClient.h"
#include "TakCot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

TakTrackers tak_trackers;

static void copyCap(char* dest, size_t n, const char* src) {
  if (!dest || n == 0) return;
  if (!src) {
    dest[0] = 0;
    return;
  }
  strncpy(dest, src, n - 1);
  dest[n - 1] = 0;
}

static bool eqI(const char* a, const char* b) {
  if (!a || !b) return false;
  while (*a && *b) {
    char ca = *a++;
    char cb = *b++;
    if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
    if (ca != cb) return false;
  }
  return *a == 0 && *b == 0;
}

static bool styleUsable(const TakCotStyle& s) { return s.type[0] != 0; }

const char* TakTrackers::sourceName(TakStyleSource src) {
  switch (src) {
    case TakStyleSource::Override: return "override";
    case TakStyleSource::Role: return "role";
    case TakStyleSource::Channel: return "channel";
    default: return "default";
  }
}

void TakTrackers::resolve(const TakPrefs& prefs, int channel, const char* id, const char* role, const char* sender,
                          const TakCotStyle*& style, TakStyleSource& src, char* callsign, size_t callsign_len) {
  const char* name = (sender && sender[0]) ? sender : "Tracker";
  copyCap(callsign, callsign_len, name);
  style = &prefs.cot;
  src = TakStyleSource::Default;

  if (id && id[0]) {
    for (int i = 0; i < TAK_MAX_TRACKER_OVR; i++) {
      const TakTrackerOverride& o = prefs.tracker_ovr[i];
      if (!o.used || !eqI(o.uid, id)) continue;
      if (o.callsign[0]) copyCap(callsign, callsign_len, o.callsign);
      if (styleUsable(o.cot)) {
        style = &o.cot;
        src = TakStyleSource::Override;
        return;
      }
      break;
    }
  }

  if (role && role[0]) {
    for (int i = 0; i < TAK_MAX_ROLES; i++) {
      const TakRoleStyle& r = prefs.roles[i];
      if (!r.enabled || !eqI(r.key, role) || !styleUsable(r.cot)) continue;
      style = &r.cot;
      src = TakStyleSource::Role;
      return;
    }
  }

  if (channel >= 0 && channel < TAK_MAX_CHAT && styleUsable(prefs.tracker_ch[channel].cot)) {
    style = &prefs.tracker_ch[channel].cot;
    src = TakStyleSource::Channel;
  }
}

void TakTrackers::clear() {
  memset(_rows, 0, sizeof(_rows));
  _count = 0;
  stats = TakTrackerStats();
}

const TakTrackerRecord* TakTrackers::at(int idx) const {
  if (idx < 0 || idx >= _count) return nullptr;
  return &_rows[idx];
}

TakTrackerRecord* TakTrackers::find(const char* id) {
  for (int i = 0; i < _count; i++) {
    if (_rows[i].valid && strcmp(_rows[i].id, id) == 0) return &_rows[i];
  }
  return nullptr;
}

TakTrackerRecord* TakTrackers::allocate(uint32_t now_ms) {
  (void)now_ms;
  if (_count < TAK_MAX_TRACKERS) return &_rows[_count++];
  int oldest = 0;
  for (int i = 1; i < _count; i++) {
    if (_rows[i].last_heard_ms < _rows[oldest].last_heard_ms) oldest = i;
  }
  memset(&_rows[oldest], 0, sizeof(_rows[oldest]));
  return &_rows[oldest];
}

static bool expired(const TakTrackerRecord& rec, time_t now) {
  if (!rec.cot_time || !now || !rec.stale_sec) return false;
  return now >= rec.cot_time + (time_t)rec.stale_sec;
}

void TakTrackers::trySend(TakClient& client, TakTrackerRecord& rec) {
  if (!rec.valid || !rec.needs_send) return;
  time_t now = client.nowUtc();
  if (!now) return;
  if (!rec.cot_time) {
    uint32_t age_s = (millis() - rec.last_heard_ms) / 1000UL;
    if (rec.msg_unix && llabs((long long)now - (long long)rec.msg_unix) <= TAK_TRACKER_TIME_SKEW_SEC) {
      rec.cot_time = (time_t)rec.msg_unix;
      rec.time_fallback = false;
    } else if (age_s >= rec.stale_sec) {
      rec.needs_send = false;
      return;
    } else {
      rec.cot_time = now - (time_t)age_s;
      rec.time_fallback = true;
    }
  }
  if (expired(rec, now)) {
    rec.needs_send = false;
    return;
  }
  if (!client.hasConfig()) return;
  const TakPrefs& prefs = client.config();
  if (rec.channel >= TAK_MAX_CHAT || !prefs.tracker_ch[rec.channel].enabled) {
    rec.needs_send = false;
    return;
  }
  const TakCotStyle* style = nullptr;
  TakStyleSource src = TakStyleSource::Default;
  resolve(prefs, rec.channel, rec.id, rec.role, rec.sender, style, src, rec.callsign, sizeof(rec.callsign));
  copyCap(rec.style_src, sizeof(rec.style_src), sourceName(src));
  if (!style || !client.tlsConnected()) return;
  if (client.queueTrackerPoint(rec, *style)) {
    rec.needs_send = false;
    rec.last_sent_ms = millis();
    stats.sent++;
  }
}

void TakTrackers::ingest(TakClient* client, const TakPrefs& prefs, int channel, const char* sender, const char* text,
                         uint32_t msg_unix, float rssi, float snr) {
  stats.heard++;
  TakTrackerMessage msg;
  if (!TakTracker::parse(text, msg)) {
    stats.parse_errors++;
    Serial.printf("[TRK] parse error ch%d\n", channel);
    return;
  }

  uint32_t now_ms = millis();
  TakTrackerRecord* rec = find(msg.id);
  bool is_new = rec == nullptr;
  if (rec) {
    TakSeqResult seq = TakTracker::sequence(true, rec->sequence, msg.sequence);
    if (seq == TakSeqResult::Duplicate) {
      stats.duplicates++;
      return;
    }
    if (seq == TakSeqResult::Older) {
      stats.out_of_order++;
      return;
    }
  } else {
    rec = allocate(now_ms);
    if (!rec) return;
  }

  rec->valid = true;
  rec->needs_send = true;
  copyCap(rec->id, sizeof(rec->id), msg.id);
  snprintf(rec->uid, sizeof(rec->uid), "meshtracker-%s", msg.id);
  copyCap(rec->sender, sizeof(rec->sender), sender && sender[0] ? sender : "Tracker");
  copyCap(rec->role, sizeof(rec->role), msg.role);
  rec->channel = (uint8_t)channel;
  rec->lat = msg.lat;
  rec->lon = msg.lon;
  rec->speed_mps = msg.speed_mps;
  rec->course_deg = msg.course_deg;
  rec->altitude_m = msg.altitude_m;
  rec->battery_pct = msg.has_battery ? msg.battery_pct : -1;
  rec->has_speed = msg.has_speed;
  rec->has_course = msg.has_course;
  rec->has_altitude = msg.has_altitude;
  rec->stale_sec = msg.stale_sec;
  rec->sequence = msg.sequence;
  rec->msg_unix = msg_unix;
  rec->last_heard_ms = now_ms;
  rec->rssi = rssi;
  rec->snr = snr;

  time_t now = client ? client->nowUtc() : 0;
  time_t msg_time = (time_t)msg_unix;
  rec->time_fallback = false;
  if (now && msg_unix && llabs((long long)now - (long long)msg_time) <= TAK_TRACKER_TIME_SKEW_SEC) {
    rec->cot_time = msg_time;
  } else if (now) {
    rec->cot_time = now;
    rec->time_fallback = true;
  } else {
    rec->cot_time = 0;
    rec->time_fallback = true;
  }

  const TakCotStyle* style = nullptr;
  TakStyleSource src = TakStyleSource::Default;
  char call[TAK_CALLSIGN_LEN];
  resolve(prefs, channel, rec->id, rec->role, rec->sender, style, src, call, sizeof(call));
  copyCap(rec->callsign, sizeof(rec->callsign), call);
  copyCap(rec->style_src, sizeof(rec->style_src), sourceName(src));
  stats.accepted++;
  Serial.printf("[TRK] %s %s k=%s %s seq=%lu%s\n", rec->callsign, rec->uid, rec->role, rec->style_src,
                (unsigned long)rec->sequence, is_new ? " new" : "");
  if (client && prefs.enabled) trySend(*client, *rec);
}

void TakTrackers::markStyleDirty(time_t now) {
  for (int i = 0; i < _count; i++) {
    TakTrackerRecord& rec = _rows[i];
    if (!rec.valid) continue;
    if (expired(rec, now)) continue;
    rec.needs_send = true;
  }
}

void TakTrackers::flush(TakClient& client) {
  if (!client.hasConfig() || !client.config().enabled) return;
  for (int i = 0; i < _count; i++) trySend(client, _rows[i]);
}
