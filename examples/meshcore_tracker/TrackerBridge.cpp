#include "TrackerBridge.h"

#include <MyMesh.h>
#include <string.h>

#include "TrackerConfig.h"
#include "TrackerMessage.h"
#include "TrackerMotion.h"

static TrackerMotion motion;
static TrackerConfig cfg;
static uint32_t sequence = 0;
static uint32_t next_sample_ms = 0;
static bool announced = false;

static bool channelKeyed(const ChannelDetails& ch) {
  for (int i = 0; i < 32; i++) {
    if (ch.channel.secret[i] != 0) return true;
  }
  return false;
}

static int pickChannel(MyMesh& mesh) {
  ChannelDetails ch;
  if (mesh.getChannel(cfg.channel, ch) && channelKeyed(ch)) return cfg.channel;
  for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
    if (mesh.getChannel(i, ch) && channelKeyed(ch)) return i;
  }
  return -1;
}

static int batteryPercent() {
  int mv = board.getBattMilliVolts();
  if (mv < 3000 || mv > 4500) return -1;
  int pct = (mv - 3300) * 100 / 900;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return pct;
}

void trackerBridgeBegin(MyMesh& mesh) {
  cfg = trackerDefaults();
  motion.setConfig(cfg.motion);

  NodePrefs* prefs = mesh.getNodePrefs();
  bool changed = false;
  if (!prefs->gps_enabled) {
    prefs->gps_enabled = 1;
    changed = true;
  }
  if (prefs->gps_interval == 0 || prefs->gps_interval > 2) {
    prefs->gps_interval = 1;
    changed = true;
  }
  if (prefs->advert_loc_policy != ADVERT_LOC_NONE) {
    prefs->advert_loc_policy = ADVERT_LOC_NONE;
    changed = true;
  }
  mesh.applyGpsPrefs();
  if (changed) mesh.savePrefs();

  char id[9];
  trackerIdFromPublicKey(mesh.self_id.pub_key, id);
  Serial.println("MeshCoreTracker");
  Serial.printf("uid %s role %s name %s\n", id, cfg.role, prefs->node_name);
  Serial.printf("radio %.3f MHz  SF%d  BW %.1f  CR%d\n", prefs->freq, prefs->sf, prefs->bw, prefs->cr);
  Serial.println("advert location off, GPS on");

  bool any = false;
  for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
    ChannelDetails ch;
    if (!mesh.getChannel(i, ch) || !channelKeyed(ch)) continue;
    any = true;
    Serial.printf("channel %d %s%s\n", i, ch.name, i == pickChannel(mesh) ? "  <- tracker" : "");
  }
  if (!any) {
    Serial.println("no private channel yet. Set one in the MeshCore app, same key as the gateway.");
  }
  Serial.println("Bluetooth PIN 123456");
}

void trackerBridgeLoop(MyMesh& mesh) {
  uint32_t now_ms = millis();
  if ((int32_t)(now_ms - next_sample_ms) < 0) return;
  next_sample_ms = now_ms + 1000;

  LocationProvider* gps = sensors.getLocationProvider();
  bool valid = gps && gps->isValid();
  TrackerFix fix;
  fix.valid = valid;
  fix.lat = valid ? sensors.node_lat : 0;
  fix.lon = valid ? sensors.node_lon : 0;
  fix.speed_mps = 0;
  TrackerDecision decision = motion.onFix(fix, now_ms / 1000);
  if (!decision.send) {
    if (!announced && !valid) {
      Serial.println("waiting for GPS fix");
      announced = true;
    }
    return;
  }

  int slot = pickChannel(mesh);
  ChannelDetails ch;
  if (slot < 0 || !mesh.getChannel(slot, ch)) {
    Serial.println("fix ready, no private channel");
    return;
  }

  uint32_t unix_s = rtc_clock.getCurrentTime();
  if (unix_s > 1700000000UL && unix_s > sequence) sequence = unix_s;
  sequence++;

  NodePrefs* prefs = mesh.getNodePrefs();
  TrackerReport report;
  memset(&report, 0, sizeof(report));
  trackerIdFromPublicKey(mesh.self_id.pub_key, report.id);
  memcpy(report.role, cfg.role, sizeof(report.role));
  report.lat = sensors.node_lat;
  report.lon = sensors.node_lon;
  report.altitude_m = (float)sensors.node_altitude;
  report.has_altitude = true;
  report.battery_pct = batteryPercent();
  report.stale_sec = decision.stale_s;
  report.sequence = sequence;

  char body[128];
  size_t n = trackerFormatFix(body, sizeof(body), report);
  if (n == 0) {
    Serial.println("tracker packet rejected by formatter");
    return;
  }

  uint32_t stamp = unix_s > 1700000000UL ? unix_s : (uint32_t)(now_ms / 1000);
  if (!mesh.sendGroupMessage(stamp, ch.channel, prefs->node_name, body, (int)n)) {
    Serial.println("tracker send failed");
    return;
  }
  announced = true;
  Serial.printf("sent slot %d %s: %s\n", slot, prefs->node_name, body);
}
