#include "TakDisplay.h"
#include "TakText.h"
#include <WiFi.h>
#include <target.h>

// 128x64: 16 px header (title + page), then four 8 px text rows.
static const int ROW[4] = {20, 31, 42, 53};

void TakDisplay::begin() {
#ifdef DISPLAY_CLASS
  // display already begun in main
#endif
}

void TakDisplay::setRefs(TakConfig* cfg, TakClient* client, TakNodes* nodes) {
  _cfg = cfg;
  _client = client;
  _nodes = nodes;
}

void TakDisplay::showBootAp(const char* ssid, const char* password) {
#ifdef DISPLAY_CLASS
  display.startFrame();
  drawHeader("SETUP");
  display.setCursor(0, ROW[0]);
  display.print(ssid ? ssid : "");
  display.setCursor(0, ROW[1]);
  display.print("PWD:");
  display.print(password ? password : "");
  display.setCursor(0, ROW[2]);
  display.print("192.168.4.1");
  display.endFrame();
#else
  (void)ssid;
  (void)password;
#endif
}

void TakDisplay::drawHeader(const char* page) {
#ifdef DISPLAY_CLASS
  int pw = display.getTextWidth(page);
  const char* title = (_cfg && _cfg->prefs.ui_title[0]) ? _cfg->prefs.ui_title : "TAK Gateway";
  display.drawTextEllipsized(0, 4, display.width() - pw - 4, title);
  display.drawTextRightAlign(display.width() - 1, 4, page);
  display.fillRect(0, 16, display.width(), 1);
#else
  (void)page;
#endif
}

void TakDisplay::drawTak() {
#ifdef DISPLAY_CLASS
  char line[40];
  display.startFrame();
  drawHeader("TAK");
  display.drawTextEllipsized(0, ROW[0], display.width(),
                             (_cfg && _cfg->prefs.tak_host[0]) ? _cfg->prefs.tak_host : "(no server)");
  display.setCursor(0, ROW[1]);
  display.print(_client ? _client->stateName() : "?");
  const TakNodeRecord* last = _nodes ? _nodes->lastHeard() : nullptr;
  if (last) {
    snprintf(line, sizeof(line), "last %s", last->name);
    display.drawTextEllipsized(0, ROW[2], display.width(), line);
    snprintf(line, sizeof(line), "%lus ago", (unsigned long)((millis() - last->last_heard_ms) / 1000UL));
    display.setCursor(0, ROW[3]);
    display.print(line);
  } else {
    display.setCursor(0, ROW[2]);
    display.print("last -");
  }
  display.endFrame();
#endif
}

bool TakDisplay::takeLongPress() {
  bool p = _long_pending;
  _long_pending = false;
  return p;
}

void TakDisplay::drawWifi() {
#ifdef DISPLAY_CLASS
  char line[40];
  bool sta = WiFi.status() == WL_CONNECTED;
  display.startFrame();
  drawHeader("WIFI");
  if (WiFi.getMode() & WIFI_AP) {
    display.drawTextEllipsized(0, ROW[0], display.width(), "MeshCore-TAK-Setup");
    snprintf(line, sizeof(line), "PWD:%s", (_cfg && _cfg->prefs.ap_password[0]) ? _cfg->prefs.ap_password : "?");
    display.drawTextEllipsized(0, ROW[1], display.width(), line);
    display.setCursor(0, ROW[2]);
    display.print("192.168.4.1");
    if (sta) snprintf(line, sizeof(line), "LAN %s", WiFi.localIP().toString().c_str());
    else if (_cfg && _cfg->hasWifi()) snprintf(line, sizeof(line), "no link: %s", _cfg->prefs.wifi_ssid);
    else line[0] = 0;
    display.drawTextEllipsized(0, ROW[3], display.width(), line);
  } else if (sta) {
    display.drawTextEllipsized(0, ROW[0], display.width(), WiFi.SSID().c_str());
    display.setCursor(0, ROW[1]);
    display.print(WiFi.localIP().toString().c_str());
    snprintf(line, sizeof(line), "RSSI %d", WiFi.RSSI());
    display.setCursor(0, ROW[2]);
    display.print(line);
  } else {
    snprintf(line, sizeof(line), "joining %s", _cfg ? _cfg->prefs.wifi_ssid : "");
    display.drawTextEllipsized(0, ROW[0], display.width(), line);
    display.setCursor(0, ROW[2]);
    display.print("hold button 3s:");
    display.setCursor(0, ROW[3]);
    display.print("open setup AP");
  }
  display.endFrame();
#endif
}

void TakDisplay::drawRadio() {
#ifdef DISPLAY_CLASS
  char line[32];
  display.startFrame();
  drawHeader("RADIO");

  uint16_t mv = board.getBattMilliVolts();
  int pct = (int)(((int)mv - 3000) * 100 / 1200);
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  snprintf(line, sizeof(line), "BAT %d%% %umV", pct, (unsigned)mv);
  display.setCursor(0, ROW[0]);
  display.print(line);

  if (_cfg) {
    snprintf(line, sizeof(line), "%s %.3f", _cfg->prefs.preset, (double)_cfg->prefs.lora_freq);
    display.setCursor(0, ROW[1]);
    display.print(line);
    snprintf(line, sizeof(line), "BW%.1f SF%d CR%d", (double)_cfg->prefs.lora_bw, (int)_cfg->prefs.lora_sf,
             (int)_cfg->prefs.lora_cr);
    display.setCursor(0, ROW[2]);
    display.print(line);
  }
  display.endFrame();
#endif
}

void TakDisplay::drawLast() {
#ifdef DISPLAY_CLASS
  char line[40];
  display.startFrame();
  drawHeader("NODES");
  if (_nodes) {
    snprintf(line, sizeof(line), "rx%lu adv%lu gps%lu", (unsigned long)_nodes->rx.packets,
             (unsigned long)_nodes->rx.adverts, (unsigned long)_nodes->rx.adverts_gps);
    display.setCursor(0, ROW[0]);
    display.print(line);
  }
  const TakNodeRecord* last = _nodes ? _nodes->lastHeard() : nullptr;
  if (last) {
    snprintf(line, sizeof(line), "%d: %s", _nodes->count(), last->name);
    display.drawTextEllipsized(0, ROW[1], display.width(), line);
    snprintf(line, sizeof(line), "%.4f %.4f", last->lat, last->lon);
    display.setCursor(0, ROW[2]);
    display.print(line);
  } else if (_nodes && _nodes->rx.last_advert_name[0]) {
    snprintf(line, sizeof(line), "no gps: %s", _nodes->rx.last_advert_name);
    display.drawTextEllipsized(0, ROW[1], display.width(), line);
  }
  if (_nodes && _nodes->rx.packets) {
    snprintf(line, sizeof(line), "rssi %.0f snr %.1f", _nodes->rx.last_rssi, _nodes->rx.last_snr);
    display.setCursor(0, ROW[3]);
    display.print(line);
  }
  display.endFrame();
#endif
}

void TakDisplay::drawChat() {
#ifdef DISPLAY_CLASS
  char line[40];
  display.startFrame();
  drawHeader("CHAT");
  display.setCursor(0, ROW[0]);
  if (!_client || !_client->chatEnabled()) {
    display.print("no channel set up");
    display.setCursor(0, ROW[1]);
    display.print("web page > Chat");
    display.endFrame();
    return;
  }
  snprintf(line, sizeof(line), "mesh>TAK %lu  TAK>mesh %lu", (unsigned long)_client->chat.mesh_to_tak,
           (unsigned long)_client->chat.tak_to_mesh);
  display.drawTextEllipsized(0, ROW[0], display.width(), line);
  const char* last = _client->chat.last();
  if (last[0]) {
    // wrap the last message over the remaining rows, 21 bytes each, never splitting a character
    const char* p = last;
    for (int r = 1; r < 4 && *p; r++) {
      char seg[22];
      utf8Copy(seg, sizeof(seg), p);
      if (!seg[0]) break;
      display.setCursor(0, ROW[r]);
      display.print(seg);
      p += strlen(seg);
    }
  }
  display.endFrame();
#endif
}

void TakDisplay::draw() {
  switch (_page) {
    case PageWifi: drawWifi(); break;
    case PageRadio: drawRadio(); break;
    case PageLast: drawLast(); break;
    case PageChat: drawChat(); break;
    default: drawTak(); break;
  }
}

void TakDisplay::loop() {
#ifdef DISPLAY_CLASS
  bool down = user_btn.isPressed();
  if (down && !_btn_was) {
    _btn_down = millis();
    _long_fired = false;
  }
  if (down && !_long_fired && millis() - _btn_down >= LONG_PRESS_MS) {
    _long_fired = true;
    _long_pending = true;
    _page = PageWifi;
    draw();
  }
  if (!down && _btn_was && !_long_fired && millis() - _btn_down >= 50) {
    _page = (Page)((_page + 1) % PageCount);
    draw();
  }
  _btn_was = down;

  // a new chat message jumps to the CHAT page
  if (_client && _client->chat.last_ms != _seen_chat_ms) {
    _seen_chat_ms = _client->chat.last_ms;
    _page = PageChat;
    draw();
  }

  if (millis() - _last_draw > 1000) {
    _last_draw = millis();
    draw();
  }
#endif
}
