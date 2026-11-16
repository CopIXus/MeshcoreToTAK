#include "TakOtaHealth.h"

#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <string.h>

namespace TakOtaHealth {
namespace {

Preferences prefs;
bool nvs_ok = false;
bool may_write_v10 = true;  // first boot / already-valid image
char skipped[16] = {0};
char fail_reason[64] = {0};
uint8_t attempts = 0;

constexpr const char* NVS_NS = "tak_ota";

void loadSkip() {
  skipped[0] = 0;
  fail_reason[0] = 0;
  attempts = 0;
  if (!nvs_ok) return;
  String v = prefs.getString("skip", "");
  if (v.length() && v.length() < sizeof(skipped)) strncpy(skipped, v.c_str(), sizeof(skipped) - 1);
  String r = prefs.getString("why", "");
  if (r.length() && r.length() < sizeof(fail_reason)) strncpy(fail_reason, r.c_str(), sizeof(fail_reason) - 1);
  attempts = (uint8_t)prefs.getUChar("tries", 0);
}

}  // namespace

void begin() {
  nvs_ok = prefs.begin(NVS_NS, false);
  loadSkip();
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  if (running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
      state == ESP_OTA_IMG_PENDING_VERIFY) {
    may_write_v10 = false;
    Serial.println("[OTA] image pending verify - config stays on prior layout until healthy");
  } else {
    may_write_v10 = true;
  }
}

bool pendingVerify() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  if (!running || esp_ota_get_state_partition(running, &state) != ESP_OK) return false;
  return state == ESP_OTA_IMG_PENDING_VERIFY;
}

bool markValid() {
  esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  if (err != ESP_OK) {
    Serial.printf("[OTA] mark valid failed: %s\n", esp_err_to_name(err));
    return false;
  }
  may_write_v10 = true;
  Serial.println("[OTA] firmware marked valid");
  return true;
}

bool rollbackNow(const char* reason) {
  Serial.printf("[OTA] rolling back: %s\n", reason ? reason : "?");
  if (nvs_ok && reason) {
    prefs.putString("why", reason);
    strncpy(fail_reason, reason, sizeof(fail_reason) - 1);
    fail_reason[sizeof(fail_reason) - 1] = 0;
  }
  esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
  // Does not return on success.
  Serial.printf("[OTA] rollback failed: %s\n", esp_err_to_name(err));
  return false;
}

void noteAttempt(const char* version) {
  if (!version || !version[0] || !nvs_ok) return;
  if (strcmp(skipped, version) == 0) {
    if (attempts < 255) attempts++;
  } else {
    strncpy(skipped, version, sizeof(skipped) - 1);
    skipped[sizeof(skipped) - 1] = 0;
    attempts = 1;
    fail_reason[0] = 0;
  }
  prefs.putString("skip", skipped);
  prefs.putUChar("tries", attempts);
  prefs.putString("last", version);
}

bool shouldSkip(const char* version) {
  if (!version || !version[0] || !skipped[0]) return false;
  return strcmp(skipped, version) == 0 && attempts >= 2;
}

const char* skippedVersion() { return skipped; }
const char* lastFailReason() { return fail_reason; }
uint8_t attemptCount(const char* version) {
  if (!version || strcmp(skipped, version) != 0) return 0;
  return attempts;
}

bool mayWriteV10() { return may_write_v10; }
void setMayWriteV10(bool ok) { may_write_v10 = ok; }

}  // namespace TakOtaHealth

// Arduino-ESP32 marks the image valid at boot unless this returns true.
extern "C" bool verifyRollbackLater() { return true; }
