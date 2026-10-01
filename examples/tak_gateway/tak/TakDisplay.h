#pragma once

#include <Arduino.h>
#include "TakConfig.h"
#include "TakClient.h"
#include "TakNodes.h"

class TakDisplay {
public:
  enum Page : uint8_t { PageTak = 0, PageWifi = 1, PageRadio = 2, PageLast = 3, PageChat = 4, PageCount = 5 };

  void begin();
  void loop();
  void showBootAp(const char* ssid, const char* password);
  void setRefs(TakConfig* cfg, TakClient* client, TakNodes* nodes);

private:
  TakConfig* _cfg = nullptr;
  TakClient* _client = nullptr;
  TakNodes* _nodes = nullptr;
  Page _page = PageWifi;
  unsigned long _last_draw = 0;
  unsigned long _btn_down = 0;
  bool _btn_was = false;
  unsigned long _seen_chat_ms = 0;

  void draw();
  void drawHeader(const char* page);
  void drawChat();
  void drawTak();
  void drawWifi();
  void drawRadio();
  void drawLast();
};
