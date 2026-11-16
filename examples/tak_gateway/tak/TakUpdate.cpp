#include "TakUpdate.h"
#include "TakClient.h"
#include "TakConfig.h"
#include "TakOtaHealth.h"
#include "TakUpdateRoots.h"
#include "TakVersion.h"
#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <stdarg.h>

#ifndef TAK_UPDATE_REPO
#define TAK_UPDATE_REPO "CopIXus/MeshcoreToTAK"
#endif
#ifndef TAK_UPDATE_ASSET
#define TAK_UPDATE_ASSET "tak_gateway_heltec_v3.bin"
#endif

#define RELEASE_URL "https://github.com/" TAK_UPDATE_REPO "/releases/latest"
#define LATEST_DOWNLOAD_URL RELEASE_URL "/download/"
#define TAG_DOWNLOAD_PREFIX "https://github.com/" TAK_UPDATE_REPO "/releases/download/"

static const unsigned long CHECK_EVERY_MS = 6UL * 3600UL * 1000UL;
static const unsigned long RETRY_MS = 3600UL * 1000UL;
static const uint32_t MIN_TLS_BLOCK = 40000;

TakUpdate tak_update;

void TakUpdate::begin(TakClient* client, TakConfig* cfg) {
  _client = client;
  _cfg = cfg;
}

const char* TakUpdate::current() { return TAK_GW_VERSION; }
const char* TakUpdate::releaseUrl() { return RELEASE_URL; }
const char* TakUpdate::skippedVersion() const { return TakOtaHealth::skippedVersion(); }
const char* TakUpdate::skipReason() const { return TakOtaHealth::lastFailReason(); }
uint8_t TakUpdate::autoMode() const {
  return _cfg ? _cfg->prefs.auto_update : TAK_AUTO_UPDATE_AUTO;
}

// YY.MMDD.HHMM is fixed width, so a plain string compare orders versions by time.
static bool validVersion(const String& v) {
  if (v.length() != 12 || v[2] != '.' || v[7] != '.') return false;
  for (int i = 0; i < 12; i++) {
    if (i != 2 && i != 7 && !isDigit(v[i])) return false;
  }
  return true;
}

bool TakUpdate::available() const {
  if (!_latest[0] || strcmp(_latest, current()) <= 0) return false;
  if (TakOtaHealth::shouldSkip(_latest)) return false;
  return true;
}

const char* TakUpdate::stateName() const {
  switch (_state) {
    case TakUpdState::Checking: return "checking";
    case TakUpdState::Installing: return "installing";
    case TakUpdState::Rebooting: return "rebooting";
    case TakUpdState::Failed: return "failed";
    default: return "idle";
  }
}

void TakUpdate::setMsg(TakUpdState s, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(_msg, sizeof(_msg), fmt, ap);
  va_end(ap);
  _state = s;
  Serial.printf("[OTA] %s\n", _msg);
}

String TakUpdate::downloadUrl(const char* asset) const {
  // Pin to the version we just read so a newer release cannot race the install.
  if (_latest[0] && validVersion(String(_latest))) {
    return String(TAG_DOWNLOAD_PREFIX) + _latest + "/" + asset;
  }
  return String(LATEST_DOWNLOAD_URL) + asset;
}

void TakUpdate::loop() {
  if (_reboot_at && (long)(millis() - _reboot_at) > 0) ESP.restart();
  if (_running || _uploading || _state == TakUpdState::Rebooting) return;
  if (WiFi.status() != WL_CONNECTED || time(nullptr) < 1700000000) return;
  if ((long)(millis() - _next_check_ms) >= 0) {
    uint8_t mode = autoMode();
    if (mode == TAK_AUTO_UPDATE_OFF) {
      _next_check_ms = millis() + CHECK_EVERY_MS;
      return;
    }
    start(mode == TAK_AUTO_UPDATE_AUTO ? Job::CheckAndMaybeInstall : Job::Check);
  }
}

bool TakUpdate::start(Job job) {
  if (_running || _uploading || _state == TakUpdState::Rebooting) return false;
  _job = job;
  _running = true;
  // Idle priority: the TLS handshake is seconds of CPU and must time-slice with IDLE0,
  // which feeds the task watchdog.
  if (xTaskCreatePinnedToCore(taskEntry, "tak_ota", 12288, this, tskIDLE_PRIORITY, nullptr, 0) != pdPASS) {
    _running = false;
    return false;
  }
  return true;
}

bool TakUpdate::requestCheck() { return start(Job::Check); }

bool TakUpdate::requestInstall(String& why) {
  if (!available()) {
    if (_latest[0] && TakOtaHealth::shouldSkip(_latest)) {
      why = String("Skipped bad version ") + _latest;
    } else {
      why = _latest[0] ? "Already running the latest version" : "Check for updates first";
    }
    return false;
  }
  if (!start(Job::Install)) {
    why = "Update busy";
    return false;
  }
  why = String("Installing ") + _latest + " - the gateway restarts when done";
  return true;
}

void TakUpdate::taskEntry(void* arg) {
  TakUpdate* u = (TakUpdate*)arg;
  String err;
  bool ok = false;
  if (u->_job == Job::Check || u->_job == Job::CheckAndMaybeInstall) {
    ok = u->doCheck(err);
    if (ok && u->_job == Job::CheckAndMaybeInstall && u->available()) {
      TakOtaHealth::noteAttempt(u->_latest);
      ok = u->doInstall(err);
      if (!ok) {
        // Attempt already counted; a second failure of the same version skips it.
      }
    }
  } else {
    TakOtaHealth::noteAttempt(u->_latest);
    ok = u->doInstall(err);
  }
  if (!ok) {
    u->setMsg(TakUpdState::Failed, "%s", err.c_str());
    if (u->_job != Job::Install) u->_next_check_ms = millis() + RETRY_MS;
  }
  if ((!ok || u->_job == Job::Check || (u->_job == Job::CheckAndMaybeInstall && !u->available())) && u->_client) {
    u->_client->pause(false);
  }
  u->_running = false;
  vTaskDelete(nullptr);
}

void TakUpdate::freeTakLinks() {
  if (!_client) return;
  _client->pause(true);  // loop() drops the TAK TLS session and frees its heap
  for (int i = 0; i < 100 && !_client->idle(); i++) vTaskDelay(pdMS_TO_TICKS(50));
}

// GET with manual redirects: GitHub sends release downloads to another host.
static int openUrl(HTTPClient& http, WiFiClientSecure& tls, String url) {
  static const char* keep[] = {"Location"};
  for (int hop = 0; hop < 5; hop++) {
    http.end();
    if (!http.begin(tls, url)) return -1;
    http.setReuse(false);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http.setUserAgent(String("MeshCoreTAKGateway/") + TAK_GW_VERSION);
    http.setConnectTimeout(15000);
    http.setTimeout(20000);
    http.collectHeaders(keep, 1);
    int code = http.GET();
    if (code < 300 || code >= 400) return code;
    url = http.header("Location");
    if (!url.startsWith("https://")) return code;
  }
  return -2;
}

static void secure(WiFiClientSecure& tls) {
  tls.setCACert(TAK_UPDATE_ROOTS);
  tls.setHandshakeTimeout(30);
}

bool TakUpdate::doCheck(String& err) {
  setMsg(TakUpdState::Checking, "Checking GitHub for updates");
  if (WiFi.status() != WL_CONNECTED) {
    err = "No Wi-Fi";
    return false;
  }
  // No PSRAM: a second TLS session next to the TAK link exhausts the heap.
  freeTakLinks();
  if (ESP.getMaxAllocHeap() < MIN_TLS_BLOCK) {
    err = String("Not enough memory to check (") + ESP.getMaxAllocHeap() + " B free block)";
    return false;
  }
  WiFiClientSecure tls;
  secure(tls);
  HTTPClient http;
  // version.txt always comes from latest so we learn the newest tag, then pin the .bin download.
  int code = openUrl(http, tls, String(LATEST_DOWNLOAD_URL) + "version.txt");
  if (code != 200) {
    http.end();
    err = code == 404 ? String("No release published on GitHub yet")
                      : String("Update check failed (") + (code < 0 ? http.errorToString(code) : String("HTTP ") + code) + ")";
    return false;
  }
  String body = http.getString();
  http.end();

  // version.txt: version, MD5 of the .bin, size of the .bin (one per line); optional SHA-256 on line 4
  String lines[4];
  int n = 0, from = 0;
  while (n < 4 && from <= (int)body.length()) {
    int nl = body.indexOf('\n', from);
    if (nl < 0) nl = body.length();
    lines[n] = body.substring(from, nl);
    lines[n].trim();
    n++;
    from = nl + 1;
  }
  if (!validVersion(lines[0])) {
    err = "GitHub release has no valid version.txt";
    return false;
  }
  strncpy(_latest, lines[0].c_str(), sizeof(_latest) - 1);
  _md5[0] = 0;
  if (lines[1].length() == 32) strncpy(_md5, lines[1].c_str(), sizeof(_md5) - 1);
  _size = lines[2].toInt();
  _checked_ms = millis();
  _next_check_ms = _checked_ms + CHECK_EVERY_MS;
  if (TakOtaHealth::shouldSkip(_latest)) {
    setMsg(TakUpdState::Idle, "Skipping bad version %s", _latest);
    return true;
  }
  int cmp = strcmp(_latest, current());
  setMsg(TakUpdState::Idle, cmp > 0 ? "Update %s available" : cmp < 0 ? "Running a newer build than GitHub (%s)" : "Up to date (%s)",
         _latest);
  return true;
}

bool TakUpdate::doInstall(String& err) {
  setMsg(TakUpdState::Installing, "Preparing update %s", _latest);
  _progress = 0;
  freeTakLinks();
  WiFiClientSecure tls;
  secure(tls);
  HTTPClient http;
  int code = openUrl(http, tls, downloadUrl(TAK_UPDATE_ASSET));
  if (code != 200) {
    http.end();
    err = String("Download failed (") + (code < 0 ? http.errorToString(code) : String("HTTP ") + code) + ")";
    return false;
  }
  int len = http.getSize();
  if (len <= 0 || (_size && (uint32_t)len != _size)) {
    http.end();
    err = "Download size does not match the release";
    return false;
  }
  if (!Update.begin(len, U_FLASH)) {
    http.end();
    err = String("Cannot start update: ") + Update.errorString();
    return false;
  }
  if (_md5[0]) Update.setMD5(_md5);
  setMsg(TakUpdState::Installing, "Downloading %s", _latest);

  WiFiClient* s = http.getStreamPtr();
  uint8_t buf[2048];  // on the update task's stack, which only exists while an update runs
  int done = 0;
  unsigned long last = millis();
  while (done < len) {
    int avail = s->available();
    if (avail > 0) {
      int n = s->readBytes(buf, min(avail, (int)sizeof(buf)));
      if (n > 0) {
        if (Update.write(buf, n) != (size_t)n) break;
        done += n;
        _progress = (int)((int64_t)done * 100 / len);
        last = millis();
      }
    } else if (!s->connected() || millis() - last > 30000) {
      break;
    } else {
      vTaskDelay(pdMS_TO_TICKS(2));
    }
  }
  http.end();
  if (done < len) {
    err = Update.hasError() ? String("Flash write failed: ") + Update.errorString() : String("Download interrupted");
    Update.abort();
    return false;
  }
  if (!Update.end()) {
    err = String("Update rejected: ") + Update.errorString();
    return false;
  }
  setMsg(TakUpdState::Rebooting, "Installed %s, restarting", _latest);
  vTaskDelay(pdMS_TO_TICKS(1500));
  ESP.restart();
  return true;
}

bool TakUpdate::uploadBegin(size_t total, String& err) {
  if (_running || _uploading) {
    err = "Update busy";
    return false;
  }
  if (!Update.begin(total ? total : UPDATE_SIZE_UNKNOWN, U_FLASH)) {
    err = String("Cannot start update: ") + Update.errorString();
    return false;
  }
  _uploading = true;
  _progress = 0;
  setMsg(TakUpdState::Installing, "Receiving uploaded firmware");
  return true;
}

bool TakUpdate::uploadWrite(const uint8_t* data, size_t len, size_t index, size_t total) {
  if (!_uploading) return false;
  if (Update.write((uint8_t*)data, len) != len) return false;
  if (total) _progress = (int)((uint64_t)(index + len) * 100 / total);
  return true;
}

bool TakUpdate::uploadEnd(String& err) {
  if (!_uploading) {
    err = "No upload in progress";
    return false;
  }
  _uploading = false;
  if (!Update.end(true)) {
    err = String("Update rejected: ") + Update.errorString();
    setMsg(TakUpdState::Failed, "%s", err.c_str());
    return false;
  }
  TakOtaHealth::noteAttempt("upload");
  setMsg(TakUpdState::Rebooting, "Uploaded firmware installed, restarting");
  _reboot_at = millis() + 1500;
  return true;
}

void TakUpdate::uploadAbort() {
  if (!_uploading) return;
  Update.abort();
  _uploading = false;
  setMsg(TakUpdState::Failed, "Upload failed");
}
