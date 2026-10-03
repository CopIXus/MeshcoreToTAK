#pragma once

#include <Arduino.h>
#include "TakConfig.h"
#include "TakClient.h"

class TakWeb {
public:
  void begin(TakConfig* cfg, TakClient* client, void (*on_radio_changed)() = nullptr);
  void loop();
  void startSetupAp();
  // Setup AP that stays up for AP_HOLD_MS even while the LAN is connected (button hold, serial "ap").
  void openSetupAp();
  void startStation();
  void stopAp();
  bool apActive() const { return _ap_active; }

private:
  static const unsigned long AP_FALLBACK_MS = 60000UL;
  static const unsigned long AP_HOLD_MS = 10UL * 60000UL;
  TakConfig* _cfg = nullptr;
  TakClient* _client = nullptr;
  void (*_on_radio_changed)() = nullptr;
  bool _ap_active = false;
  bool _server_started = false;
  unsigned long _ap_hold_until = 0;
  unsigned long _sta_up_since = 0;
  unsigned long _sta_down_since = 0;
  unsigned long _last_wifi_check = 0;

  void setupRoutes();
  String statusJson() const;
  String configJson() const;
};
