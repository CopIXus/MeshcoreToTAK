#pragma once

#include <Arduino.h>
#include "TakConfig.h"
#include "TakClient.h"

class TakWeb {
public:
  void begin(TakConfig* cfg, TakClient* client, void (*on_radio_changed)() = nullptr);
  void loop();
  void startSetupAp();
  void startStation();
  void stopAp();
  bool apActive() const { return _ap_active; }

private:
  TakConfig* _cfg = nullptr;
  TakClient* _client = nullptr;
  void (*_on_radio_changed)() = nullptr;
  bool _ap_active = false;
  bool _server_started = false;

  void setupRoutes();
  String statusJson() const;
  String configJson() const;
};
