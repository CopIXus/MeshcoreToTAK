#pragma once

#include <Arduino.h>

// Pending-verify / rollback bookkeeping for OTA. Stored in NVS so both the new
// and the previous firmware can read it after a rollback.
namespace TakOtaHealth {

enum class AutoUpdate : uint8_t { Off = 0, Notify = 1, Automatic = 2 };

void begin();
// True while the bootloader is waiting for esp_ota_mark_app_valid_cancel_rollback().
bool pendingVerify();
// Radio, config, Wi-Fi and NTP must all succeed before this is called.
bool markValid();
// Immediate rollback to the previous slot (only when pendingVerify()).
bool rollbackNow(const char* reason);

// Remember an attempted OTA version so a bad build is not reinstalled forever.
void noteAttempt(const char* version);
bool shouldSkip(const char* version);
const char* skippedVersion();
const char* lastFailReason();
uint8_t attemptCount(const char* version);

// Config may write the v10 layout only after the image is marked valid.
bool mayWriteV10();
void setMayWriteV10(bool ok);

}  // namespace TakOtaHealth
