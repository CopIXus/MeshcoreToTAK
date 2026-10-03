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
      } else if (line == "ap") {
        tak_web.openSetupAp();
        Serial.printf("OK setup AP MeshCore-TAK-Setup pwd=%s -> http://192.168.4.1\n", tak_config.prefs.ap_password);
      } else if (line.length()) {
        Serial.println("cmds: status | fs | ap | factory_reset");
      }
      line = "";
    } else {
      if (line.length() < 500) line += c;
    }
  }
}

void halt() {
  while (1) delay(1000);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\nMeshCore TAK Gateway v%sZ\n", TAK_GW_VERSION);

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
    Serial.println("SPIFFS mount failed");
    halt();
  }

  if (!radio_init()) {
    Serial.println("Radio init failed");
    halt();
  }

  fast_rng.begin(radio_driver.getRngSeed());

  bool had = tak_config.load();
  Serial.printf("[CFG] loaded=%d ssid=%s host=%s preset=%s\n", had, tak_config.prefs.wifi_ssid,
                tak_config.prefs.tak_host, tak_config.prefs.preset);
  if (!had) tak_config.save();

  tak_nodes.clear();
  the_mesh.beginFs();

  tak_client.begin(&tak_config, &tak_nodes);
  tak_web.begin(&tak_config, &tak_client, onRadioChanged);
  tak_update.begin(&tak_client);
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
