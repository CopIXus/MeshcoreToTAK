#include "TakWeb.h"
#include "TakNodes.h"
#include "TakCerts.h"
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <SPIFFS.h>
#include <target.h>
#include "TakWebAssets.h"
#include "TakText.h"
#include "TakUpdate.h"
#include <Utils.h>

extern void onChatConfigChanged();
extern void requestMeshAdvert();
extern unsigned long lastMeshAdvertMs();
extern uint32_t meshAdvertsSent();

static AsyncWebServer* g_server = nullptr;
static TakWeb* g_web = nullptr;
static TakConfig* g_cfg = nullptr;
static TakClient* g_client = nullptr;
static void (*g_radio_cb)() = nullptr;
static AsyncAuthenticationMiddleware g_auth;

// Body callbacks run before middleware, so uploads must check credentials themselves;
// the middleware then answers 401 once the body has been consumed.
static bool authed(AsyncWebServerRequest* req) { return g_auth.allowed(req); }

static void applyPassword(const char* pwd) {
  g_auth.setPassword(pwd && pwd[0] ? pwd : "meshcore");
  g_auth.generateHash();
}

static void sendGz(AsyncWebServerRequest* req, const char* type, const uint8_t* data, size_t len,
                   bool gz, const char* cache) {
  AsyncWebServerResponse* r = req->beginResponse_P(200, type, data, len);
  if (gz) r->addHeader("Content-Encoding", "gzip");
  r->addHeader("Cache-Control", cache);
  req->send(r);
}

// Flat-object JSON lookup for exact key match. Handles quoted (with escapes) and bare values.
static bool jsonGet(const String& body, const char* key, String& out) {
  out = "";
  String needle = String("\"") + key + "\"";
  int p = 0;
  while ((p = body.indexOf(needle, p)) >= 0) {
    int i = p + needle.length();
    while (i < (int)body.length() && body[i] == ' ') i++;
    if (i >= (int)body.length() || body[i] != ':') {
      p++;
      continue;
    }
    i++;
    while (i < (int)body.length() && body[i] == ' ') i++;
    if (i < (int)body.length() && body[i] == '"') {
      i++;
      while (i < (int)body.length()) {
        char c = body[i++];
        if (c == '"') return true;
        if (c == '\\' && i < (int)body.length()) {
          char n = body[i++];
          if (n == 'n') out += '\n';
          else if (n == 't') out += '\t';
          else if (n == 'u') i += 4;
          else out += n;
          continue;
        }
        out += c;
      }
      return true;
    }
    int e = i;
    while (e < (int)body.length() && body[e] != ',' && body[e] != '}') e++;
    out = body.substring(i, e);
    out.trim();
    return true;
  }
  return false;
}

static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Returns bytes decoded, or -1 if s is not an even run of hex digits that fits out.
static int parseHex(const String& s, uint8_t* out, size_t out_len) {
  if (s.length() % 2 || s.length() / 2 > out_len) return -1;
  for (size_t i = 0; i < s.length(); i += 2) {
    int hi = hexVal(s[i]), lo = hexVal(s[i + 1]);
    if (hi < 0 || lo < 0) return -1;
    out[i / 2] = (uint8_t)(hi << 4 | lo);
  }
  return s.length() / 2;
}

static bool isHexColor(const String& v) {
  if (v.length() != 7 || v[0] != '#') return false;
  for (int i = 1; i < 7; i++) {
    if (hexVal(v[i]) < 0) return false;
  }
  return true;
}

void TakWeb::begin(TakConfig* cfg, TakClient* client, void (*on_radio_changed)()) {
  _cfg = cfg;
  _client = client;
  _on_radio_changed = on_radio_changed;
  g_web = this;
  g_cfg = cfg;
  g_client = client;
  g_radio_cb = on_radio_changed;
  if (!g_server) g_server = new AsyncWebServer(80);
  g_auth.setAuthType(AsyncAuthType::AUTH_DIGEST);
  g_auth.setRealm("MeshCore TAK Gateway");
  g_auth.setUsername("admin");
  applyPassword(cfg ? cfg->prefs.setup_password : nullptr);
  g_server->addMiddleware(&g_auth);
  setupRoutes();
}

void TakWeb::setupRoutes() {
  g_server->on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    sendGz(req, "text/html", TAK_INDEX_HTML_GZ, TAK_INDEX_HTML_GZ_LEN, true, "no-cache");
  });

  g_server->on("/icons.png", HTTP_GET, [](AsyncWebServerRequest* req) {
    sendGz(req, "image/png", TAK_ICONS_PNG, TAK_ICONS_PNG_LEN, false, "max-age=604800");
  });

  g_server->on("/iconset.png", HTTP_GET, [](AsyncWebServerRequest* req) {
    sendGz(req, "image/png", TAK_ICONSET_PNG, TAK_ICONSET_PNG_LEN, false, "max-age=604800");
  });

  g_server->on("/api/status", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!g_web) {
      req->send(500, "text/plain", "no web");
      return;
    }
    req->send(200, "application/json", g_web->statusJson());
  });

  g_server->on("/api/config", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!g_web) {
      req->send(500, "text/plain", "no web");
      return;
    }
    req->send(200, "application/json", g_web->configJson());
  });

  g_server->on(
      "/api/config", HTTP_POST,
      [](AsyncWebServerRequest* req) {},
      nullptr,
      [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
        if (!authed(req)) return;
        if (!g_cfg) {
          req->send(500, "text/plain", "no cfg");
          return;
        }
        static String body;
        if (index == 0) body = "";
        for (size_t i = 0; i < len; i++) body += (char)data[i];
        if (index + len < total) return;

        TakPrefs& p = g_cfg->prefs;
        // static: TakPrefs is too large for this task's stack once the filters are in it
        static TakPrefs before;
        before = p;
        String v;

        // Required fields keep their old value when blank; optional ones may be cleared.
        auto setc = [&](char* dest, size_t n, const char* key, bool allow_empty) {
          if (!jsonGet(body, key, v)) return;
          if (!v.length() && !allow_empty) return;
          strncpy(dest, v.c_str(), n - 1);
          dest[n - 1] = 0;
        };
        auto setu = [&](uint16_t& dest, const char* key, long lo, long hi) {
          if (!jsonGet(body, key, v) || !v.length()) return;
          long x = v.toInt();
          dest = (uint16_t)(x < lo ? lo : (x > hi ? hi : x));
        };
        auto setb = [&](bool& dest, const char* key) {
          if (jsonGet(body, key, v) && v.length()) dest = v.toInt() != 0;
        };

        setc(p.wifi_ssid, sizeof(p.wifi_ssid), "wifi_ssid", false);
        setc(p.wifi_psk, sizeof(p.wifi_psk), "wifi_psk", true);
        String pw_err;
        if (jsonGet(body, "setup_password", v) && v.length()) {
          if (v.length() < 6 || v.length() >= sizeof(p.setup_password)) {
            pw_err = "\nWeb password not changed: use 6 to " + String(sizeof(p.setup_password) - 1) + " characters";
          } else {
            strcpy(p.setup_password, v.c_str());
          }
        }
        setc(p.tak_host, sizeof(p.tak_host), "tak_host", false);
        setc(p.channel_label, sizeof(p.channel_label), "channel_label", true);
        setc(p.cot.type, sizeof(p.cot.type), "cot_type", false);
        setc(p.cot.how, sizeof(p.cot.how), "cot_how", false);
        setc(p.cot.remarks, sizeof(p.cot.remarks), "cot_remarks", true);
        setc(p.cot.icon, sizeof(p.cot.icon), "cot_icon", true);
        if (jsonGet(body, "cot_color", v) && v.length() == 7 && v[0] == '#') {
          strncpy(p.cot.marker_color, v.c_str(), sizeof(p.cot.marker_color) - 1);
        }
        if (jsonGet(body, "cot_opacity", v) && v.length()) {
          int o = constrain(v.toInt(), 10, 100);
          p.cot.marker_opacity = o / 100.0f;
        }

        setu(p.tak_port, "tak_port", 1, 65535);
        setu(p.stale_sec, "stale_sec", 10, 65535);
        setu(p.refresh_sec, "refresh_sec", 10, 65535);
        setu(p.max_age_sec, "max_age_sec", 60, 65535);
        setb(p.enabled, "enabled");
        setb(p.cot.archived, "cot_archived");

        // ---- unit filters ----
        setb(p.send_unmatched, "send_unmatched");
        for (int i = 0; i < TAK_MAX_FILTERS; i++) {
          TakUnitFilter& f = p.filters[i];
          char k[20];
          snprintf(k, sizeof(k), "f%d_on", i);
          setb(f.enabled, k);
          snprintf(k, sizeof(k), "f%d_label", i);
          setc(f.label, sizeof(f.label), k, true);
          snprintf(k, sizeof(k), "f%d_mode", i);
          if (jsonGet(body, k, v) && v.length()) f.mode = (uint8_t)constrain(v.toInt(), 0, 2);
          snprintf(k, sizeof(k), "f%d_match", i);
          setc(f.match, sizeof(f.match), k, true);
          snprintf(k, sizeof(k), "f%d_strip", i);
          setb(f.strip, k);
          snprintf(k, sizeof(k), "f%d_type", i);
          setc(f.cot.type, sizeof(f.cot.type), k, false);
          snprintf(k, sizeof(k), "f%d_how", i);
          setc(f.cot.how, sizeof(f.cot.how), k, false);
          snprintf(k, sizeof(k), "f%d_remarks", i);
          setc(f.cot.remarks, sizeof(f.cot.remarks), k, true);
          snprintf(k, sizeof(k), "f%d_icon", i);
          setc(f.cot.icon, sizeof(f.cot.icon), k, true);
          snprintf(k, sizeof(k), "f%d_color", i);
          if (jsonGet(body, k, v) && v.length() == 7 && v[0] == '#') {
            strncpy(f.cot.marker_color, v.c_str(), sizeof(f.cot.marker_color) - 1);
          }
          snprintf(k, sizeof(k), "f%d_opacity", i);
          if (jsonGet(body, k, v) && v.length()) {
            f.cot.marker_opacity = constrain(v.toInt(), 10, 100) / 100.0f;
          }
          snprintf(k, sizeof(k), "f%d_archived", i);
          setb(f.cot.archived, k);
        }

        // ---- customization ----
        setc(p.ui_title, sizeof(p.ui_title), "ui_title", false);
        setc(p.banner_text, sizeof(p.banner_text), "banner_text", true);
        if (jsonGet(body, "banner_color", v) && isHexColor(v)) strcpy(p.banner_color, v.c_str());
        if (jsonGet(body, "accent", v) && isHexColor(v)) strcpy(p.accent, v.c_str());
        setb(p.banner_on, "banner_on");
        // ---- chat bridge ----
        setc(p.chat_callsign, sizeof(p.chat_callsign), "chat_callsign", false);
        if (jsonGet(body, "chat_lat", v)) p.chat_lat = constrain(v.toFloat(), -90.0f, 90.0f);
        if (jsonGet(body, "chat_lon", v)) p.chat_lon = constrain(v.toFloat(), -180.0f, 180.0f);
        setb(p.public_on, "public_on");
        setc(p.public_room, sizeof(p.public_room), "public_room", false);
        // ---- mesh advert ----
        setc(p.node_name, sizeof(p.node_name), "node_name", false);
        setb(p.advert_on, "advert_on");
        setu(p.advert_hours, "advert_hours", 1, 168);
        String chat_err;
        for (int i = 0; i < TAK_MAX_CHAT; i++) {
          TakChatChannel& c = p.chat[i];
          char k[16];
          snprintf(k, sizeof(k), "ch%d_name", i);
          if (!jsonGet(body, k, v)) continue;
          v.trim();
          bool renamed = strcmp(v.c_str(), c.name) != 0;
          strncpy(c.name, v.c_str(), sizeof(c.name) - 1);
          c.name[sizeof(c.name) - 1] = 0;
          if (!c.name[0]) {
            memset(&c, 0, sizeof(c));
            continue;
          }
          snprintf(k, sizeof(k), "ch%d_key", i);
          if (jsonGet(body, k, v) && v.length()) {
            v.trim();
            uint8_t sec[32];
            int n = parseHex(v, sec, sizeof(sec));
            if (n == 16 || n == 32) {
              memset(c.secret, 0, sizeof(c.secret));
              memcpy(c.secret, sec, n);
              c.secret_len = n;
            } else {
              chat_err += String("\nChannel ") + c.name + ": key must be 32 or 64 hex characters";
            }
          } else if (c.name[0] == '#' && (renamed || !c.secret_len)) {
            // MeshCore hashtag channels: key = first 16 bytes of sha256("#name")
            memset(c.secret, 0, sizeof(c.secret));
            mesh::Utils::sha256(c.secret, 16, (const uint8_t*)c.name, strlen(c.name));
            c.secret_len = 16;
          }
          snprintf(k, sizeof(k), "ch%d_room", i);
          if (jsonGet(body, k, v)) {
            v.trim();
            strncpy(c.room, v.length() ? v.c_str() : (c.name[0] == '#' ? c.name + 1 : c.name), sizeof(c.room) - 1);
            c.room[sizeof(c.room) - 1] = 0;
          }
          snprintf(k, sizeof(k), "ch%d_on", i);
          setb(c.enabled, k);
          if (c.enabled && !c.secret_len) chat_err += String("\nChannel ") + c.name + ": needs its secret key";
        }

        if (jsonGet(body, "preset", v) && v.length()) {
          g_cfg->applyPreset(v.c_str());
          if (strcasecmp(v.c_str(), "CUSTOM") == 0) {
            if (jsonGet(body, "lora_freq", v) && v.length()) p.lora_freq = v.toFloat();
            if (jsonGet(body, "lora_bw", v) && v.length()) p.lora_bw = v.toFloat();
            if (jsonGet(body, "lora_sf", v) && v.length()) p.lora_sf = v.toInt();
            if (jsonGet(body, "lora_cr", v) && v.length()) p.lora_cr = v.toInt();
          }
        }

        g_cfg->save();

        bool link_changed = before.enabled != p.enabled || before.tak_port != p.tak_port ||
                            strcmp(before.tak_host, p.tak_host) != 0 ||
                            strcmp(before.wifi_ssid, p.wifi_ssid) != 0 ||
                            strcmp(before.wifi_psk, p.wifi_psk) != 0;
        bool radio_changed = before.lora_freq != p.lora_freq || before.lora_bw != p.lora_bw ||
                             before.lora_sf != p.lora_sf || before.lora_cr != p.lora_cr;
        bool wifi_changed = strcmp(before.wifi_ssid, p.wifi_ssid) != 0 || strcmp(before.wifi_psk, p.wifi_psk) != 0;
        if (wifi_changed && g_web) g_web->startStation();
        if (link_changed && g_client) g_client->requestConnect();
        if (radio_changed && g_radio_cb) g_radio_cb();
        bool chat_changed = memcmp(before.chat, p.chat, sizeof(p.chat)) != 0 ||
                            strcmp(before.chat_callsign, p.chat_callsign) != 0 ||
                            before.public_on != p.public_on || strcmp(before.public_room, p.public_room) != 0 ||
                            before.chat_lat != p.chat_lat || before.chat_lon != p.chat_lon;
        if (chat_changed) onChatConfigChanged();
        bool advert_changed = (p.advert_on && !before.advert_on) ||
                              (p.advert_on && (strcmp(before.node_name, p.node_name) != 0 ||
                                               before.chat_lat != p.chat_lat || before.chat_lon != p.chat_lon));
        if (advert_changed) requestMeshAdvert();
        if (strcmp(before.setup_password, p.setup_password) != 0) {
          applyPassword(p.setup_password);
          pw_err += "\nWeb password changed - sign in again with the new password";
        }

        if (g_client) g_client->removeFiltered();  // before markAllForResend clears the sent marks
        extern TakNodes tak_nodes;
        tak_nodes.markAllForResend();  // push the new styling on the next refresh tick
        String msg = link_changed ? "Saved - reconnecting to TAK" : "Saved - markers update within a few seconds";
        req->send(200, "text/plain", msg + chat_err + pw_err);
      });

  g_server->on("/logo.png", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!g_cfg || !SPIFFS.exists(g_cfg->logoPath())) {
      req->send(404, "text/plain", "no logo");
      return;
    }
    AsyncWebServerResponse* r = req->beginResponse(SPIFFS, g_cfg->logoPath(), "image/png");
    r->addHeader("Cache-Control", "no-cache");
    req->send(r);
  });

  // Must be registered before "/api/logo", which also matches paths below it.
  g_server->on("/api/logo/delete", HTTP_POST, [](AsyncWebServerRequest* req) {
    if (g_cfg) SPIFFS.remove(g_cfg->logoPath());
    req->send(200, "text/plain", "Logo removed");
  });

  // Raw PNG body (the page resizes the image first), streamed straight to flash.
  g_server->on(
      "/api/logo", HTTP_POST,
      [](AsyncWebServerRequest* req) {},
      nullptr,
      [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
        static File f;
        static size_t written = 0;
        static const char* tmp = "/tak/logo.tmp";
        if (!g_cfg || !authed(req)) return;
        if (total > 48 * 1024) {
          if (index == 0) req->send(413, "text/plain", "Logo too large (max 48 KB after resize)");
          return;
        }
        if (index == 0) {
          if (f) f.close();
          SPIFFS.remove(tmp);
          f = SPIFFS.open(tmp, "w");
          written = 0;
        }
        if (f) written += f.write(data, len);
        if (index + len < total) return;
        String err;
        if (!f) {
          err = "Logo save failed: could not create the file";
        } else {
          f.close();
          if (written != total) {
            err = "Logo save failed: storage full (" + String(written) + " of " + String(total) + " bytes written)";
          } else {
            SPIFFS.remove(g_cfg->logoPath());
            if (!SPIFFS.rename(tmp, g_cfg->logoPath())) err = "Logo save failed: could not replace the old logo";
          }
        }
        if (err.length()) {
          SPIFFS.remove(tmp);
          err += " - " + String((SPIFFS.totalBytes() - SPIFFS.usedBytes()) / 1024) + " KB free";
          Serial.printf("[WEB] %s\n", err.c_str());
        }
        req->send(err.length() ? 500 : 200, "text/plain", err.length() ? err : String("Logo saved"));
      });

  g_server->on("/api/advert", HTTP_POST, [](AsyncWebServerRequest* req) {
    requestMeshAdvert();
    bool clock_ok = g_client && g_client->nowUtc();
    req->send(200, "text/plain", clock_ok ? "Advert sent" : "Advert queued - waiting for the clock (NTP)");
  });

  g_server->on(
      "/api/certs", HTTP_POST,
      [](AsyncWebServerRequest* req) {},
      nullptr,
      [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
        if (!authed(req)) return;
        if (!g_cfg) {
          req->send(500, "text/plain", "no cfg");
          return;
        }
        static String body;
        if (index == 0) body = "";
        for (size_t i = 0; i < len; i++) body += (char)data[i];
        if (index + len < total) return;

        auto extract = [&](const char* key) -> String {
          // Match exact JSON key "name" (not prefix of another key), allow spaces around ':'
          String needle = String("\"") + key + "\"";
          int p = 0;
          while ((p = body.indexOf(needle, p)) >= 0) {
            int after = p + needle.length();
            while (after < (int)body.length() && (body[after] == ' ' || body[after] == '\t')) after++;
            if (after >= (int)body.length() || body[after] != ':') {
              p++;
              continue;
            }
            after++;
            while (after < (int)body.length() && (body[after] == ' ' || body[after] == '\t')) after++;
            if (after >= (int)body.length() || body[after] != '"') {
              p++;
              continue;
            }
            after++;  // opening quote
            String out;
            while (after < (int)body.length()) {
              char c = body[after++];
              if (c == '\\' && after < (int)body.length()) {
                char n = body[after++];
                if (n == 'n') out += '\n';
                else if (n == 'r') out += '\r';
                else if (n == 't') out += '\t';
                else if (n == '"') out += '"';
                else if (n == '\\') out += '\\';
                else if (n == 'u') after += 4;
                else out += n;
                continue;
              }
              if (c == '"') return out;
              out += c;
            }
            return out;
          }
          return String();
        };
        TakCerts::Bundle b;
        b.ca = extract("ca");
        b.cert = extract("cert");
        b.key = extract("key");
        b.passphrase = extract("key_passphrase");
        String err;
        bool ok = TakCerts::install(g_cfg, b, err);
        if (ok && g_client) g_client->requestConnect();
        req->send(ok ? 200 : 400, "text/plain", err);
      });

  g_server->on("/api/test", HTTP_POST, [](AsyncWebServerRequest* req) {
    if (!g_client) {
      req->send(500, "text/plain", "no client");
      return;
    }
    String err;
    bool ok = g_client->testConnection(err);
    req->send(ok ? 200 : 400, "text/plain", err);
  });

  g_server->on("/api/update/check", HTTP_POST, [](AsyncWebServerRequest* req) {
    bool ok = tak_update.requestCheck();
    req->send(ok ? 200 : 409, "text/plain", ok ? "Checking GitHub for updates" : "Update busy");
  });

  g_server->on("/api/update/install", HTTP_POST, [](AsyncWebServerRequest* req) {
    String why;
    bool ok = tak_update.requestInstall(why);
    req->send(ok ? 200 : 409, "text/plain", why);
  });

  // Raw firmware .bin body (the app image, not the merged full-flash file)
  g_server->on(
      "/api/update/upload", HTTP_POST,
      [](AsyncWebServerRequest* req) {},
      nullptr,
      [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
        static bool failed = false;
        if (!authed(req)) return;
        String err;
        if (index == 0) {
          failed = !tak_update.uploadBegin(total, err);
          if (failed) {
            req->send(409, "text/plain", err);
            return;
          }
          req->onDisconnect([]() { tak_update.uploadAbort(); });
        }
        if (failed) return;
        if (!tak_update.uploadWrite(data, len, index, total)) {
          failed = true;
          tak_update.uploadAbort();
          req->send(400, "text/plain", "Firmware write failed - is this the gateway .bin?");
          return;
        }
        if (index + len < total) return;
        bool ok = tak_update.uploadEnd(err);
        req->send(ok ? 200 : 400, "text/plain", ok ? "Firmware installed - restarting" : err);
      });

  g_server->on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest* req) {
    req->send(200, "text/plain", "rebooting");
    delay(100);
    ESP.restart();
  });
}

static String jsonEsc(const String& s) {
  String o;
  o.reserve(s.length() + 8);
  const char* p = s.c_str();
  for (size_t i = 0; i < s.length();) {
    char c = p[i];
    size_t n = utf8SeqLen(p + i);
    if (!n) {
      i++;  // invalid / truncated UTF-8
      continue;
    }
    if (c == '"' || c == '\\') {
      o += '\\';
      o += c;
    } else if (c == '\n') {
      o += "\\n";
    } else if (c == '\r') {
      o += "\\r";
    } else if ((uint8_t)c < 0x20) {
      // skip control chars
    } else {
      o.concat(p + i, n);
    }
    i += n;
  }
  return o;
}

String TakWeb::statusJson() const {
  String ip = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : (_ap_active ? "192.168.4.1" : "");
  String wifi = (WiFi.status() == WL_CONNECTED) ? "UP" : (_ap_active ? "AP" : "DOWN");
  String ssid = "";
  int rssi = 0;
  if (WiFi.status() == WL_CONNECTED) {
    ssid = WiFi.SSID();
    rssi = WiFi.RSSI();
  } else if (_ap_active) {
    ssid = "MeshCore-TAK-Setup";
  }

  uint16_t batt_mv = board.getBattMilliVolts();
  int batt_pct = (int)(((int)batt_mv - 3000) * 100 / 1200);
  if (batt_pct < 0) batt_pct = 0;
  if (batt_pct > 100) batt_pct = 100;

  extern TakNodes tak_nodes;
  const TakNodeRecord* last = tak_nodes.lastHeard();
  String last_name = last ? String(last->name) : "-";
  String last_ago = "";
  if (last) {
    last_ago = String((millis() - last->last_heard_ms) / 1000UL) + "s";
  }

  String j = "{";
  j += "\"state\":\"" + jsonEsc(String(_client ? _client->stateName() : "?")) + "\",";
  j += "\"wifi\":\"" + wifi + "\",";
  j += "\"ip\":\"" + ip + "\",";
  j += "\"ssid\":\"" + jsonEsc(ssid) + "\",";
  j += "\"rssi\":" + String(rssi) + ",";
  j += "\"batt_mv\":" + String(batt_mv) + ",";
  j += "\"batt_pct\":" + String(batt_pct) + ",";
  j += "\"uptime_s\":" + String(millis() / 1000UL) + ",";
  j += "\"ntp_epoch\":" + String((unsigned long)time(nullptr)) + ",";
  j += "\"tak_host\":\"" + jsonEsc(String(_cfg ? _cfg->prefs.tak_host : "")) + "\",";
  j += "\"tak_port\":" + String(_cfg ? _cfg->prefs.tak_port : 0) + ",";
  j += "\"certs\":" + String(_cfg && _cfg->hasClientCerts() ? "true" : "false") + ",";
  j += "\"heap\":" + String(ESP.getFreeHeap()) + ",";
  if (_cfg) {
    j += "\"preset\":\"" + jsonEsc(String(_cfg->prefs.preset)) + "\",";
    j += "\"lora_freq\":" + String(_cfg->prefs.lora_freq, 3) + ",";
    j += "\"lora_bw\":" + String(_cfg->prefs.lora_bw, 1) + ",";
    j += "\"lora_sf\":" + String(_cfg->prefs.lora_sf) + ",";
    j += "\"lora_cr\":" + String(_cfg->prefs.lora_cr) + ",";
  }
  j += "\"error\":\"" + jsonEsc(String(_client ? _client->lastError() : "")) + "\",";
  const TakRxStats& rx = tak_nodes.rx;
  j += "\"rx_packets\":" + String(rx.packets) + ",";
  j += "\"rx_ago\":\"" + (rx.packets ? String((millis() - rx.last_rx_ms) / 1000UL) + "s" : String("")) + "\",";
  j += "\"rx_rssi\":" + String(rx.last_rssi, 0) + ",";
  j += "\"rx_snr\":" + String(rx.last_snr, 1) + ",";
  j += "\"adverts\":" + String(rx.adverts) + ",";
  j += "\"adverts_gps\":" + String(rx.adverts_gps) + ",";
  j += "\"last_advert\":\"" + jsonEsc(String(rx.last_advert_name)) + "\",";
  j += String("\"last_advert_gps\":") + (rx.last_advert_gps ? "true" : "false") + ",";
  j += "\"nodes\":" + String(_cfg ? tak_nodes.sentCount(_cfg->prefs) : tak_nodes.count()) + ",";
  // GPS nodes heard (name, seconds ago) so the page can test filter rules live
  j += "\"gps_nodes\":[";
  for (int i = 0; i < tak_nodes.count(); i++) {
    const TakNodeRecord* n = tak_nodes.at(i);
    if (!n) continue;
    if (i) j += ",";
    j += "[\"" + jsonEsc(String(n->name)) + "\"," + String((millis() - n->last_heard_ms) / 1000UL) + "]";
  }
  j += "],";
  j += "\"last_name\":\"" + jsonEsc(last_name) + "\",";
  j += "\"last_ago\":\"" + last_ago + "\",";
  if (_client) {
    const TakChatStats& c = _client->chat;
    j += "\"chat_m2t\":" + String(c.mesh_to_tak) + ",";
    j += "\"chat_t2m\":" + String(c.tak_to_mesh) + ",";
    j += "\"chat_recent\":[";
    for (int i = 0; i < c.recent_n; i++) {
      const TakChatMsg& m = c.recent[i];
      if (i) j += ",";
      j += String("{\"m\":") + (m.from_mesh ? "true" : "false") + ",\"ago\":" + String((millis() - m.ms) / 1000UL) +
           ",\"t\":\"" + jsonEsc(String(m.text)) + "\"}";
    }
    j += "],";
    j += "\"gw_uid\":\"" + String(_client->gatewayUid()) + "\",";
  }
  auto ago = [](unsigned long ms) { return ms ? String((millis() - ms) / 1000UL) : String("null"); };
  if (_client) {
    const TakLinkStats& l = _client->link;
    bool up = _client->tlsConnected();
    j += "\"tak_up\":" + (up ? ago(l.up_since_ms) : String("null")) + ",";
    j += "\"tak_connects\":" + String(l.connects) + ",";
    j += "\"tak_points\":" + String(l.points) + ",";
    j += "\"tak_point_ago\":" + ago(l.last_point_ms) + ",";
    j += "\"tak_point_name\":\"" + jsonEsc(String(l.last_point)) + "\",";
    j += "\"tak_removed\":" + String(l.removed) + ",";
    j += "\"tak_chats\":" + String(l.chats) + ",";
    j += "\"tak_events\":" + String(l.events) + ",";
    j += "\"tak_dropped\":" + String(l.dropped) + ",";
    j += "\"tak_queue\":" + String(_client->queued()) + ",";
    j += "\"tak_tx_ago\":" + ago(l.last_tx_ms) + ",";
    j += "\"tak_rx_ago\":" + ago(l.last_rx_ms) + ",";
    j += "\"chat_ago\":" + ago(_client->chat.last_ms) + ",";
  }
  j += "\"rx_last_ago\":" + ago(rx.last_rx_ms) + ",";
  j += "\"adv_heard_ago\":" + ago(rx.last_advert_ms) + ",";
  j += "\"gps_ago\":" + ago(rx.last_gps_ms) + ",";
  j += "\"gps_heard\":" + String(tak_nodes.count()) + ",";
  j += "\"my_adverts\":" + String(meshAdvertsSent()) + ",";
  if (_cfg) {
    j += String("\"advert_on\":") + (_cfg->prefs.advert_on ? "true" : "false") + ",";
    j += "\"advert_hours\":" + String(_cfg->prefs.advert_hours) + ",";
  }
  unsigned long adv = lastMeshAdvertMs();
  j += "\"advert_ago\":" + ago(adv) + ",";
  j += "\"fs_used\":" + String(SPIFFS.usedBytes()) + ",";
  j += "\"fs_total\":" + String(SPIFFS.totalBytes()) + ",";
  j += "\"fw\":\"" + String(TakUpdate::current()) + "\",";
  j += "\"upd_state\":\"" + String(tak_update.stateName()) + "\",";
  j += "\"upd_latest\":\"" + String(tak_update.latest()) + "\",";
  j += String("\"upd_avail\":") + (tak_update.available() ? "true" : "false") + ",";
  j += "\"upd_progress\":" + String(tak_update.progress()) + ",";
  j += "\"upd_msg\":\"" + jsonEsc(String(tak_update.message())) + "\",";
  j += "\"upd_checked_ago\":" + ago(tak_update.checkedMs()) + ",";
  j += "\"upd_url\":\"" + String(TakUpdate::releaseUrl()) + "\",";
  j += String("\"logo\":") + (_cfg && SPIFFS.exists(_cfg->logoPath()) ? "true" : "false");
  j += "}";
  return j;
}

String TakWeb::configJson() const {
  String j = "{";
  if (_cfg) {
    j += "\"wifi_ssid\":\"" + jsonEsc(String(_cfg->prefs.wifi_ssid)) + "\",";
    j += "\"wifi_psk\":\"" + jsonEsc(String(_cfg->prefs.wifi_psk)) + "\",";
    j += "\"tak_host\":\"" + jsonEsc(String(_cfg->prefs.tak_host)) + "\",";
    j += "\"tak_port\":\"" + String(_cfg->prefs.tak_port) + "\",";
    j += "\"channel_label\":\"" + jsonEsc(String(_cfg->prefs.channel_label)) + "\",";
    j += "\"enabled\":\"" + String(_cfg->prefs.enabled ? 1 : 0) + "\",";
    j += "\"preset\":\"" + jsonEsc(String(_cfg->prefs.preset)) + "\",";
    j += "\"lora_freq\":\"" + String(_cfg->prefs.lora_freq, 3) + "\",";
    j += "\"lora_bw\":\"" + String(_cfg->prefs.lora_bw, 1) + "\",";
    j += "\"lora_sf\":\"" + String(_cfg->prefs.lora_sf) + "\",";
    j += "\"lora_cr\":\"" + String(_cfg->prefs.lora_cr) + "\",";
    j += "\"send_unmatched\":\"" + String(_cfg->prefs.send_unmatched ? 1 : 0) + "\",";
    j += "\"stale_sec\":\"" + String(_cfg->prefs.stale_sec) + "\",";
    j += "\"refresh_sec\":\"" + String(_cfg->prefs.refresh_sec) + "\",";
    j += "\"max_age_sec\":\"" + String(_cfg->prefs.max_age_sec) + "\",";
    j += "\"cot_type\":\"" + jsonEsc(String(_cfg->prefs.cot.type)) + "\",";
    j += "\"cot_how\":\"" + jsonEsc(String(_cfg->prefs.cot.how)) + "\",";
    j += "\"cot_remarks\":\"" + jsonEsc(String(_cfg->prefs.cot.remarks)) + "\",";
    j += "\"cot_icon\":\"" + jsonEsc(String(_cfg->prefs.cot.icon)) + "\",";
    j += "\"cot_color\":\"" + jsonEsc(String(_cfg->prefs.cot.marker_color)) + "\",";
    j += "\"cot_archived\":\"" + String(_cfg->prefs.cot.archived ? 1 : 0) + "\",";
    float op = _cfg->prefs.cot.marker_opacity;
    if (!(op > 0.0f) || op > 1.0f) op = 1.0f;
    j += "\"cot_opacity\":\"" + String((int)(op * 100.0f + 0.5f)) + "\",";
    const TakPrefs& p = _cfg->prefs;
    for (int i = 0; i < TAK_MAX_FILTERS; i++) {
      const TakUnitFilter& f = p.filters[i];
      String k = "\"f" + String(i) + "_";
      j += k + "on\":\"" + String(f.enabled ? 1 : 0) + "\",";
      j += k + "label\":\"" + jsonEsc(String(f.label)) + "\",";
      j += k + "mode\":\"" + String(f.mode) + "\",";
      j += k + "match\":\"" + jsonEsc(String(f.match)) + "\",";
      j += k + "strip\":\"" + String(f.strip ? 1 : 0) + "\",";
      j += k + "type\":\"" + jsonEsc(String(f.cot.type)) + "\",";
      j += k + "how\":\"" + jsonEsc(String(f.cot.how)) + "\",";
      j += k + "remarks\":\"" + jsonEsc(String(f.cot.remarks)) + "\",";
      j += k + "icon\":\"" + jsonEsc(String(f.cot.icon)) + "\",";
      j += k + "color\":\"" + jsonEsc(String(f.cot.marker_color)) + "\",";
      float fop = f.cot.marker_opacity;
      if (!(fop > 0.0f) || fop > 1.0f) fop = 1.0f;
      j += k + "opacity\":\"" + String((int)(fop * 100.0f + 0.5f)) + "\",";
      j += k + "archived\":\"" + String(f.cot.archived ? 1 : 0) + "\",";
    }
    j += "\"ui_title\":\"" + jsonEsc(String(p.ui_title)) + "\",";
    j += "\"banner_text\":\"" + jsonEsc(String(p.banner_text)) + "\",";
    j += "\"banner_color\":\"" + jsonEsc(String(p.banner_color)) + "\",";
    j += "\"accent\":\"" + jsonEsc(String(p.accent)) + "\",";
    j += "\"banner_on\":\"" + String(p.banner_on ? 1 : 0) + "\",";
    j += "\"chat_callsign\":\"" + jsonEsc(String(p.chat_callsign)) + "\",";
    j += "\"public_on\":\"" + String(p.public_on ? 1 : 0) + "\",";
    j += "\"public_room\":\"" + jsonEsc(String(p.public_room)) + "\",";
    j += "\"chat_lat\":\"" + (p.chat_lat || p.chat_lon ? String(p.chat_lat, 6) : String("")) + "\",";
    j += "\"chat_lon\":\"" + (p.chat_lat || p.chat_lon ? String(p.chat_lon, 6) : String("")) + "\",";
    j += "\"pw_default\":\"" + String(strcmp(p.setup_password, "meshcore") == 0 ? 1 : 0) + "\",";
    j += "\"node_name\":\"" + jsonEsc(String(p.node_name)) + "\",";
    j += "\"advert_on\":\"" + String(p.advert_on ? 1 : 0) + "\",";
    j += "\"advert_hours\":\"" + String(p.advert_hours) + "\",";
    for (int i = 0; i < TAK_MAX_CHAT; i++) {
      const TakChatChannel& c = p.chat[i];
      String k = "\"ch" + String(i) + "_";
      j += k + "on\":\"" + String(c.enabled ? 1 : 0) + "\",";
      j += k + "name\":\"" + jsonEsc(String(c.name)) + "\",";
      j += k + "room\":\"" + jsonEsc(String(c.room)) + "\",";
      j += k + "haskey\":\"" + String(c.secret_len ? 1 : 0) + "\",";  // the key itself is write-only
    }
    j += "\"key_passphrase\":\"" + jsonEsc(String(_cfg->prefs.key_passphrase)) + "\"";
  }
  j += "}";
  return j;
}

void TakWeb::startSetupAp() {
  if (!_cfg) return;
  WiFi.mode(WIFI_AP_STA);
  String ssid = "MeshCore-TAK-Setup";
  WiFi.softAP(ssid.c_str(), _cfg->prefs.ap_password);
  _ap_active = true;
  if (!_server_started) {
    g_server->begin();
    _server_started = true;
  }
  Serial.printf("[WEB] Setup AP %s pwd=%s -> http://192.168.4.1\n", ssid.c_str(), _cfg->prefs.ap_password);
}

void TakWeb::startStation() {
  if (!_cfg || !_cfg->hasWifi()) return;
  WiFi.mode(_ap_active ? WIFI_AP_STA : WIFI_STA);
  WiFi.begin(_cfg->prefs.wifi_ssid, _cfg->prefs.wifi_psk);
  Serial.printf("[WEB] STA connecting to %s\n", _cfg->prefs.wifi_ssid);
  if (!_server_started) {
    g_server->begin();
    _server_started = true;
  }
}

void TakWeb::stopAp() {
  if (_ap_active) {
    WiFi.softAPdisconnect(true);
    _ap_active = false;
    if (_cfg && _cfg->hasWifi()) WiFi.mode(WIFI_STA);
  }
}

void TakWeb::loop() {
  // Async server needs no polling; optionally stop AP after STA up
  static unsigned long last = 0;
  if (millis() - last < 2000) return;
  last = millis();
  if (_ap_active && WiFi.status() == WL_CONNECTED) {
    // Drop SoftAP as soon as STA is up so outbound TAK sockets route cleanly
    static unsigned long sta_since = 0;
    if (!sta_since) sta_since = millis();
    if (millis() - sta_since > 5000UL) {
      stopAp();
      sta_since = 0;
    }
  }
}
