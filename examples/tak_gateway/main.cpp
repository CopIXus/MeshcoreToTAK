#include <Arduino.h>
#include <Mesh.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <target.h>

#include "MyMesh.h"
#include "tak/TakConfig.h"
#include "tak/TakNodes.h"
#include "tak/TakClient.h"
#include "tak/TakWeb.h"
#include "tak/TakDisplay.h"
#include "tak/TakUpdate.h"
#include "tak/TakOtaHealth.h"
#include "tak/TakVersion.h"

// The TAK TLS handshake runs on loop(); 8 KB left under 2 KB spare.
SET_LOOP_TASK_STACK_SIZE(12 * 1024);

StdRNG fast_rng;
SimpleMeshTables tables;

TakConfig tak_config;
TakNodes tak_nodes;
TakClient tak_client;
TakWeb tak_web;
TakDisplay tak_display;

MyMesh the_mesh(radio_driver, fast_rng, rtc_clock, tables, tak_config, tak_nodes, tak_client);

static void onRadioChanged() { the_mesh.applyRadioFromConfig(); }

void onChatConfigChanged() { the_mesh.applyChannelsFromConfig(); }

void requestMeshAdvert() { the_mesh.requestAdvert(); }

unsigned long lastMeshAdvertMs() { return the_mesh.lastAdvertMs(); }

uint32_t meshAdvertsSent() { return the_mesh.advertsSent(); }

// Main loop passes per second and the slowest pass, over the last 10 s window.
static const uint32_t LOOP_WINDOW_MS = 10000;
static volatile uint32_t g_loop_hz = 0;
static volatile uint32_t g_loop_worst_us = 0;
uint32_t loopRateHz() { return g_loop_hz; }
uint32_t loopWorstUs() { return g_loop_worst_us; }

static void noteLoopPass(uint32_t us) {
  static uint32_t n = 0, worst = 0;
  static unsigned long start = millis();
  n++;
  if (us > worst) worst = us;
  unsigned long now = millis();
  if (now - start >= LOOP_WINDOW_MS) {
    g_loop_hz = (uint32_t)((uint64_t)n * 1000UL / (now - start));
    g_loop_worst_us = worst;
    n = 0;
    worst = 0;
    start = now;
  }
}

static void handleSerial() {
  static String line;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (line.startsWith("status")) {
        Serial.printf("version=%s state=%s host=%s wifi=%d ip=%s err=%s\n", TAK_GW_VERSION, tak_client.stateName(),
                      tak_config.prefs.tak_host, WiFi.status() == WL_CONNECTED,
                      WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "-",
                      tak_client.lastError());
        Serial.printf("uptime=%lus heap=%u min_heap=%u rssi=%d gw=%s dns=%s net_ok=%lus ago\n", millis() / 1000,
                      (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(), WiFi.RSSI(),
                      WiFi.gatewayIP().toString().c_str(), WiFi.dnsIP().toString().c_str(),
                      tak_client.netOkMs() ? (millis() - tak_client.netOkMs()) / 1000 : 0);
        Serial.printf("flood hops unscoped=%lu scoped=%lu last_scope_code=%04x\n",
                      (unsigned long)tak_nodes.rx.flood_hops_unscoped, (unsigned long)tak_nodes.rx.flood_hops_scoped,
                      tak_nodes.rx.last_scoped_code);
        TaskHandle_t web = xTaskGetHandle("async_tcp");
        Serial.printf("stack_free loop=%u web=%u queued=%d\n", (unsigned)uxTaskGetStackHighWaterMark(nullptr),
                      web ? (unsigned)uxTaskGetStackHighWaterMark(web) : 0, tak_client.queued());
      } else if (line.startsWith("fs")) {
        Serial.printf("SPIFFS used %u of %u bytes\n", (unsigned)SPIFFS.usedBytes(), (unsigned)SPIFFS.totalBytes());
        File root = SPIFFS.open("/");
        for (File f = root.openNextFile(); f; f = root.openNextFile()) Serial.printf("  %6u %s\n", (unsigned)f.size(), f.path());
      } else if (line.startsWith("factory_reset")) {
        tak_config.factoryResetNetworkAndTak();
        Serial.println("OK factory reset, restarting");
        delay(500);
        ESP.restart();
      } else if (line == "regions" || line.startsWith("regions ")) {
        String arg = line.length() > 8 ? line.substring(8) : String();
        arg.trim();
        the_mesh.setRegionProbe(arg.c_str());
      } else if (line == "ap") {
        tak_web.openSetupAp();
        Serial.printf("OK setup AP MeshCore-TAK-Setup pwd=%s -> http://192.168.4.1\n", tak_config.prefs.ap_password);
      } else if (line.startsWith("name ")) {
        String name = line.substring(5);
        name.trim();
        if (!name.length() || name.length() >= sizeof(tak_config.prefs.node_name)) {
          Serial.println("name must be 1 to 31 characters");
        } else {
          strncpy(tak_config.prefs.node_name, name.c_str(), sizeof(tak_config.prefs.node_name) - 1);
          tak_config.prefs.node_name[sizeof(tak_config.prefs.node_name) - 1] = 0;
          tak_config.save();
          requestMeshAdvert();
          Serial.printf("OK name %s\n", tak_config.prefs.node_name);
        }
      } else if (line.startsWith("wifi ")) {
        String rest = line.substring(5);
        rest.trim();
        int sp = rest.indexOf(' ');
        if (sp <= 0) {
          Serial.println("usage: wifi <ssid> <password>");
        } else {
          String ssid = rest.substring(0, sp);
          String psk = rest.substring(sp + 1);
          psk.trim();
          if (ssid.length() >= sizeof(tak_config.prefs.wifi_ssid) || psk.length() >= sizeof(tak_config.prefs.wifi_psk)) {
            Serial.println("ssid or password is too long");
          } else {
            strncpy(tak_config.prefs.wifi_ssid, ssid.c_str(), sizeof(tak_config.prefs.wifi_ssid) - 1);
            tak_config.prefs.wifi_ssid[sizeof(tak_config.prefs.wifi_ssid) - 1] = 0;
            strncpy(tak_config.prefs.wifi_psk, psk.c_str(), sizeof(tak_config.prefs.wifi_psk) - 1);
            tak_config.prefs.wifi_psk[sizeof(tak_config.prefs.wifi_psk) - 1] = 0;
            tak_config.save();
            tak_web.startStation();
            Serial.printf("OK wifi %s\n", tak_config.prefs.wifi_ssid);
          }
        }
      } else if (line.startsWith("tak ")) {
        String rest = line.substring(4);
        rest.trim();
        int sp = rest.indexOf(' ');
        String host = sp > 0 ? rest.substring(0, sp) : rest;
        long port = sp > 0 ? rest.substring(sp + 1).toInt() : 8089;
        host.trim();
        if (!host.length() || host.length() >= sizeof(tak_config.prefs.tak_host) || port < 1 || port > 65535) {
          Serial.println("usage: tak <host> <port>");
        } else {
          strncpy(tak_config.prefs.tak_host, host.c_str(), sizeof(tak_config.prefs.tak_host) - 1);
          tak_config.prefs.tak_host[sizeof(tak_config.prefs.tak_host) - 1] = 0;
          tak_config.prefs.tak_port = (uint16_t)port;
          tak_config.prefs.enabled = true;
          tak_config.save();
          if (tak_client.tlsConnected() || tak_client.state() != TakLinkState::Disabled) {
            tak_client.reconnect();
          } else {
            tak_client.requestConnect();
          }
          Serial.printf("OK tak %s:%u\n", tak_config.prefs.tak_host, (unsigned)tak_config.prefs.tak_port);
        }
      } else if (line.startsWith("group ") || line == "group") {
        String name = line.length() > 6 ? line.substring(6) : String();
        name.trim();
        if (!name.length() || name.length() >= sizeof(tak_config.prefs.channel_label)) {
          Serial.println("usage: group <TAK channel>");
        } else {
          strncpy(tak_config.prefs.channel_label, name.c_str(), sizeof(tak_config.prefs.channel_label) - 1);
          tak_config.prefs.channel_label[sizeof(tak_config.prefs.channel_label) - 1] = 0;
          // Markers use __group. Chat uses the same name so it does not stay in __ANON__.
          for (int i = 0; i < TAK_MAX_CHAT; i++) {
            if (!tak_config.prefs.chat[i].name[0]) continue;
            strncpy(tak_config.prefs.chat[i].room, name.c_str(), sizeof(tak_config.prefs.chat[i].room) - 1);
            tak_config.prefs.chat[i].room[sizeof(tak_config.prefs.chat[i].room) - 1] = 0;
          }
          tak_config.save();
          Serial.printf("OK group %s\n", tak_config.prefs.channel_label);
        }
      } else if (line.length()) {
        Serial.println("cmds: status | fs | ap | name <callsign> | wifi <ssid> <password> | tak <host> <port> | group <TAK channel> | regions name,name | factory_reset");
      }
      line = "";
    } else {
      if (line.length() < 500) line += c;
    }
  }
}

static void showFatal(const char* msg) {
#ifdef DISPLAY_CLASS
  display.startFrame();
  display.drawTextCentered(display.width() / 2, 20, "FATAL");
  display.drawTextCentered(display.width() / 2, 36, msg ? msg : "?");
  display.endFrame();
#endif
  Serial.printf("[BOOT] fatal: %s\n", msg ? msg : "?");
}

void halt(const char* reason) {
  showFatal(reason);
  if (TakOtaHealth::pendingVerify()) {
    TakOtaHealth::rollbackNow(reason ? reason : "boot failure");
  }
  delay(60000);
  ESP.restart();
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\nMeshCore TAK Gateway v%sZ\n", TAK_GW_VERSION);
  TakOtaHealth::begin();

  board.begin();
#ifdef DISPLAY_CLASS
  if (display.begin()) {
    display.startFrame();
    display.drawTextCentered(display.width() / 2, 22, "TAK Gateway");
    display.drawTextCentered(display.width() / 2, 36, "v" TAK_GW_VERSION);
    display.endFrame();
  }
  user_btn.begin();
#endif

  if (!SPIFFS.begin(true)) {
    halt("SPIFFS failed");
  }

  if (!radio_init()) {
    halt("Radio failed");
  }

  fast_rng.begin(radio_driver.getRngSeed());

  bool had = tak_config.load();
  Serial.printf("[CFG] loaded=%d failed=%d ssid=%s host=%s preset=%s name=%s\n", had, tak_config.loadFailed(),
                tak_config.prefs.wifi_ssid, tak_config.prefs.tak_host, tak_config.prefs.preset,
                tak_config.prefs.node_name);
  // Only create a fresh file on true first boot. Never overwrite a corrupt file.
  if (!had && !tak_config.loadFailed()) tak_config.save();

  tak_nodes.clear();
  the_mesh.beginFs();

  tak_client.begin(&tak_config, &tak_nodes);
  tak_web.begin(&tak_config, &tak_client, onRadioChanged);
  tak_update.begin(&tak_client, &tak_config);
  tak_display.setRefs(&tak_config, &tak_client, &tak_nodes);
  tak_display.begin();

  if (tak_config.hasWifi()) {
    tak_web.startStation();  // tak_web opens the setup AP if this does not connect
  } else {
    tak_web.startSetupAp();
    tak_display.showBootAp("MeshCore-TAK-Setup", tak_config.prefs.ap_password);
  }

  if (tak_config.prefs.enabled) tak_client.requestConnect();
  Serial.println("[BOOT] ready");
}

void loop() {
  uint32_t pass_start = micros();
  the_mesh.loopGateway();
  tak_client.loop();
  tak_web.loop();
  tak_update.loop();
  tak_display.loop();
  if (tak_display.takeLongPress()) tak_web.openSetupAp();
  handleSerial();

  // Health gate for a freshly flashed OTA image: mark valid once radio, Wi-Fi and NTP are up.
  static bool health_done = false;
  static unsigned long health_deadline = 0;
  if (!health_done && TakOtaHealth::pendingVerify()) {
    if (!health_deadline) health_deadline = millis() + 5UL * 60UL * 1000UL;
    bool wifi_ok = !tak_config.hasWifi() || WiFi.status() == WL_CONNECTED;
    bool ntp_ok = !tak_config.hasWifi() || tak_client.ntpOk() || time(nullptr) > 1700000000;
    if (wifi_ok && ntp_ok) {
      TakOtaHealth::markValid();
      // Persist v10 layout now that rollback is cancelled.
      if (!tak_config.loadFailed()) tak_config.save();
      health_done = true;
    } else if ((long)(millis() - health_deadline) >= 0) {
      // Attempt was counted when the install started; only record why we rolled back.
      TakOtaHealth::rollbackNow("health check timed out");
    }
  } else if (!health_done) {
    health_done = true;
    TakOtaHealth::setMayWriteV10(true);
  }

  // Each STA retry scans channels, which drops phones on the setup AP, so retry slower while it is up.
  static unsigned long last_wifi = 0;
  if (millis() - last_wifi > (tak_web.apActive() ? 60000UL : 10000UL)) {
    last_wifi = millis();
    if (tak_config.hasWifi() && WiFi.status() != WL_CONNECTED) {
      WiFi.reconnect();
    }
  }
  noteLoopPass(micros() - pass_start);
}
