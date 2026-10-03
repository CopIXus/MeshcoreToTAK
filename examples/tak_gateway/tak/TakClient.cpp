#include "TakClient.h"
#include "TakCot.h"
#include "TakCerts.h"
#include "TakText.h"
#include <WiFi.h>
#include <time.h>
#include <string.h>

#include "esp_tls.h"
#include "esp_err.h"
#include "mbedtls/error.h"
#include "lwip/sockets.h"

#define TLS ((esp_tls_t*)_tls)

// Outbound encode scratch, static so the loop task stack stays small.
static uint8_t s_detail[2048];
static uint8_t s_cot[3072];  // CotEvent
static uint8_t s_out[2816];  // protobuf frame or line-broken XML
static const size_t Q_ITEM_MAX = 2048;
static const uint32_t Q_MIN_HEAP = 24000;

TakClient::TakClient() {
  _last_error[0] = 0;
}

TakClient::~TakClient() { disconnectTls(); }

void TakClient::begin(TakConfig* cfg, TakNodes* nodes) {
  _cfg = cfg;
  _nodes = nodes;
  _last_error[0] = 0;
  for (char*& q : _q) {
    free(q);
    q = nullptr;
  }
  _q_head = _q_tail = _q_count = 0;
  snprintf(_gw_uid, sizeof(_gw_uid), "meshcore-gw-%012llx", (unsigned long long)(ESP.getEfuseMac() & 0xFFFFFFFFFFFFULL));
  if (_cfg && _cfg->prefs.enabled) setState(TakLinkState::WaitWifi);
  else setState(TakLinkState::Disabled);
}

void TakClient::setError(const char* msg) {
  strncpy(_last_error, msg ? msg : "", sizeof(_last_error) - 1);
  _last_error[sizeof(_last_error) - 1] = 0;
}

void TakClient::setState(TakLinkState s) { _state = s; }

static const char* linkName(TakLinkState s) {
  switch (s) {
    case TakLinkState::Disabled: return "DISABLED";
    case TakLinkState::WaitWifi: return "WAIT_WIFI";
    case TakLinkState::WaitNtp: return "WAIT_NTP";
    case TakLinkState::Connecting: return "CONNECTING";
    case TakLinkState::Connected: return "CONNECTED";
    case TakLinkState::Backoff: return "BACKOFF";
    case TakLinkState::Error: return "ERROR";
  }
  return "?";
}

const char* TakClient::stateName() const { return linkName(_state); }

void TakClient::requestConnect() {
  if (!_cfg || !_cfg->prefs.enabled) {
    setState(TakLinkState::Disabled);
    return;
  }
  _backoff_ms = 1000;
  setState(TakLinkState::WaitWifi);
}

time_t TakClient::nowUtc() const {
  time_t now = time(nullptr);
  // ESP32Board seeds the clock with 15 May 2024 at boot; only trust time after a real NTP sync.
  if (now < 1767225600) return 0;  // 2026-01-01
  return now;
}

void TakClient::disconnectTls() {
  if (_tls) {
    esp_tls_conn_destroy(TLS);
    _tls = nullptr;
  }
}

static esp_err_t formatTlsError(esp_tls_t* tls, const char* prefix, char* msg, size_t n) {
  if (tls) {
    int esp_code = 0;
    int mbedtls_code = 0;
    esp_err_t err = esp_tls_get_and_clear_last_error(tls->error_handle, &esp_code, &mbedtls_code);
    char mbed_str[48] = {0};
    if (mbedtls_code) mbedtls_strerror(mbedtls_code, mbed_str, sizeof(mbed_str));
    snprintf(msg, n, "%s esp=%s/%d mbed=%s", prefix ? prefix : "TLS", esp_err_to_name(err), esp_code,
             mbed_str[0] ? mbed_str : "none");
    return err;
  }
  snprintf(msg, n, "%s (no tls handle)", prefix ? prefix : "TLS");
  return ESP_FAIL;
}

void TakClient::captureTlsError(const char* prefix) {
  char msg[96] = {0};
  formatTlsError(TLS, prefix, msg, sizeof(msg));
  setError(msg);
  Serial.printf("[TAK] %s\n", msg);
}

bool TakClient::loadCertPems() {
  if (!_cfg || !_cfg->hasClientCerts()) {
    setError("missing cert files — upload Portal .pem + .key");
    return false;
  }
  _ca_pem = _cfg->readFile(_cfg->caPath());
  _cert_pem = _cfg->readFile(_cfg->certPath());
  _key_pem = _cfg->readFile(_cfg->keyPath());
  if (_ca_pem.isEmpty() || _cert_pem.isEmpty() || _key_pem.isEmpty()) {
    setError("empty cert files");
    return false;
  }

  String kerr;
  if (!TakCerts::preparePrivateKey(_key_pem, _cfg->prefs.key_passphrase, kerr)) {
    setError(kerr.c_str());
    return false;
  }
  if (TakCerts::keyLooksEncrypted(_cfg->readFile(_cfg->keyPath()))) {
    _cfg->writeFile(_cfg->keyPath(), _key_pem);
  }
  if (_ca_pem.indexOf("BEGIN CERTIFICATE") < 0) {
    setError("CA missing — re-upload Portal .pem (includes chain)");
    return false;
  }
  Serial.printf("[TAK] certs loaded ca=%u cert=%u key=%u heap=%u time=%ld\n", (unsigned)_ca_pem.length(),
                (unsigned)_cert_pem.length(), (unsigned)_key_pem.length(), (unsigned)ESP.getFreeHeap(),
                (long)time(nullptr));
  // The CA chain is parsed once into the esp_tls store so its PEM can be freed. Replacing the store
  // frees the old chain, so no session may be open here.
  disconnectTls();
  esp_err_t ce = esp_tls_set_global_ca_store((const unsigned char*)_ca_pem.c_str(), _ca_pem.length() + 1);
  _ca_pem = String();
  _ca_ok = ce == ESP_OK;
  if (!_ca_ok) {
    setError("CA certificate could not be parsed - re-install the certificate");
    return false;
  }
  return true;
}

bool TakClient::openSession(void*& slot, const String& cert, const String& key, uint16_t port, const char* tag,
                            char* err, size_t err_len) {
  auto note = [&](const char* m) {
    Serial.printf("[TAK] %s %s\n", tag ? tag : "tls", m);
    if (err && err_len) {
      strncpy(err, m, err_len - 1);
      err[err_len - 1] = 0;
    } else {
      setError(m);
    }
  };
  if (slot) {
    esp_tls_conn_destroy((esp_tls_t*)slot);
    slot = nullptr;
  }
  if (!_cfg || !_cfg->hasTakHost()) {
    note("no TAK host");
    return false;
  }
  if (!_ca_ok || cert.isEmpty() || key.isEmpty()) {
    note("missing certificate");
    return false;
  }

  if (!port) port = _cfg->prefs.tak_port ? _cfg->prefs.tak_port : 8089;
  Serial.printf("[TAK] %s connecting %s:%d heap=%u\n", tag, _cfg->prefs.tak_host, port, (unsigned)ESP.getFreeHeap());
  slot = esp_tls_init();
  if (!slot) {
    note("esp_tls_init failed");
    return false;
  }
  esp_tls_t* tls = (esp_tls_t*)slot;
  esp_tls_cfg_t cfg = {};
  cfg.use_global_ca_store = true;
  // PEM buffers must include the trailing NUL in the reported size. They are parsed into copies,
  // so the caller may free them once this returns.
  cfg.clientcert_buf = (const unsigned char*)cert.c_str();
  cfg.clientcert_bytes = cert.length() + 1;
  cfg.clientkey_buf = (const unsigned char*)key.c_str();
  cfg.clientkey_bytes = key.length() + 1;
  // Portal server cert SAN/CN is typically "takserver", while Integrations host is an FQDN.
  cfg.common_name = _skip_cn ? nullptr : "takserver";
  cfg.skip_common_name = _skip_cn;
  cfg.timeout_ms = 20000;
  tls_keep_alive_cfg_t ka = {};
  ka.keep_alive_enable = true;
  ka.keep_alive_idle = 60;
  ka.keep_alive_interval = 15;
  ka.keep_alive_count = 4;
  cfg.keep_alive_cfg = &ka;

  int ret = esp_tls_conn_new_sync(_cfg->prefs.tak_host, strlen(_cfg->prefs.tak_host), port, &cfg, tls);
  if (ret != 1) {
    char msg[96];
    esp_err_t why = formatTlsError(tls, "TLS handshake failed", msg, sizeof(msg));
    note(msg);
    esp_tls_conn_destroy(tls);
    slot = nullptr;
    // A host that never answered cannot have a name mismatch, and a second timeout stalls the mesh loop.
    bool unreached = why == ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME || why == ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST ||
                     why == ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT;
    if (!unreached) _net_ok_ms = millis();
    if (_skip_cn || unreached) return false;
    slot = esp_tls_init();
    if (!slot) {
      note("esp_tls_init failed (retry)");
      return false;
    }
    tls = (esp_tls_t*)slot;
    cfg.common_name = nullptr;
    cfg.skip_common_name = true;
    Serial.printf("[TAK] %s retry TLS with skip_common_name\n", tag);
    ret = esp_tls_conn_new_sync(_cfg->prefs.tak_host, strlen(_cfg->prefs.tak_host), port, &cfg, tls);
    if (ret != 1) {
      formatTlsError(tls, "TLS failed", msg, sizeof(msg));
      note(msg);
      esp_tls_conn_destroy(tls);
      slot = nullptr;
      return false;
    }
    _skip_cn = true;
  }

  _net_ok_ms = millis();
  // The handshake used a 20 s socket timeout; a read must never stall the mesh loop that long.
  int fd = -1;
  if (esp_tls_get_conn_sockfd(tls, &fd) == ESP_OK && fd >= 0) {
    struct timeval tv = {0, 100000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  }
  if (err && err_len) err[0] = 0;
  return true;
}

bool TakClient::connectTls() {
  if (!_cfg->hasTakHost()) {
    setError("no TAK host");
    return false;
  }
  if (!loadCertPems()) return false;

  setState(TakLinkState::Connecting);
  IPAddress ip;
  if (WiFi.hostByName(_cfg->prefs.tak_host, ip)) {
    Serial.printf("[TAK] DNS %s -> %s\n", _cfg->prefs.tak_host, ip.toString().c_str());
  }
  bool opened = openSession(_tls, _cert_pem, _key_pem, _cfg->prefs.tak_port, "publish", nullptr, 0);
  _cert_pem = String();
  _key_pem = String();
  if (!opened) {
    disconnectTls();
    return false;
  }

  setError("");
  _backoff_ms = 1000;
  _last_ping_ms = millis();
  _pub.clear();
  link.connects++;
  link.up_since_ms = millis();
  setState(TakLinkState::Connected);
  Serial.println("[TAK] Connected");
  return true;
}

bool TakClient::drainInbound() {
  if (!_tls) return false;
  for (int i = 0; i < 8; i++) {
    if (esp_tls_get_bytes_avail(TLS) <= 0) {
      int fd = -1;
      if (esp_tls_get_conn_sockfd(TLS, &fd) != ESP_OK || fd < 0) return false;
      fd_set rfds;
      FD_ZERO(&rfds);
      FD_SET(fd, &rfds);
      struct timeval tv = {0, 0};
      int s = select(fd + 1, &rfds, nullptr, nullptr, &tv);
      if (s < 0) return false;
      if (s == 0) return true;
    }
    int n = esp_tls_conn_read(TLS, _pub.rx + _pub.rx_len, sizeof(_pub.rx) - 1 - _pub.rx_len);
    if (n > 0) {
      link.last_rx_ms = millis();
      _pub.rx_len += n;
      _pub.rx[_pub.rx_len] = 0;
      processRx(_pub);
      continue;
    }
    if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) return true;
    return false;  // 0 = server closed, <0 = socket error / keepalive timeout
  }
  return true;
}

bool TakClient::sendProtoAsk(void* tls, TakInSock& s, const char* tag) {
  if (!tls || !s.want_proto || s.out_proto != 0 || !nowUtc()) return false;
  char uid_esc[160], t0[32], t1[32], req[512];
  time_t now = nowUtc();
  TakCot::xmlEscape(s.proto_uid, uid_esc, sizeof(uid_esc));
  TakCot::formatTime(now, t0, sizeof(t0));
  TakCot::formatTime(now + 60, t1, sizeof(t1));
  int n = snprintf(req, sizeof(req),
                   "<?xml version='1.0' encoding='UTF-8' standalone='yes'?>\n"
                   "<event version='2.0' uid='%s' type='t-x-takp-q' how='m-g' time='%s' start='%s' stale='%s'>"
                   "<point lat='0.0' lon='0.0' hae='0.0' ce='9999999.0' le='9999999.0'/>"
                   "<detail><TakControl><TakRequest version='1'/></TakControl></detail></event>",
                   uid_esc, t0, t0, t1);
  if (n <= 0 || (size_t)n >= sizeof(req)) return false;
  int w = esp_tls_conn_write((esp_tls_t*)tls, req, (size_t)n);
  if (w <= 0) return false;
  s.want_proto = false;
  s.out_proto = 1;
  s.proto_wait_ms = millis();
  Serial.printf("[TAK] %s requested TAK protocol 1\n", tag ? tag : "link");
  return true;
}

// ---------- server traffic (pings and protocol negotiation) ----------

static void xmlUnescape(const char* s, size_t len, char* out, size_t out_len) {
  size_t o = 0;
  for (size_t i = 0; i < len && o + 4 < out_len;) {
    if (s[i] != '&') {
      out[o++] = s[i++];
      continue;
    }
    const char* semi = (const char*)memchr(s + i, ';', len - i < 10 ? len - i : 10);
    if (!semi) {
      out[o++] = s[i++];
      continue;
    }
    size_t el = semi - (s + i) + 1;
    uint32_t cp = 0;
    if (!strncmp(s + i, "&amp;", 5)) cp = '&';
    else if (!strncmp(s + i, "&lt;", 4)) cp = '<';
    else if (!strncmp(s + i, "&gt;", 4)) cp = '>';
    else if (!strncmp(s + i, "&quot;", 6)) cp = '"';
    else if (!strncmp(s + i, "&apos;", 6)) cp = '\'';
    else if (s[i + 1] == '#') cp = (s[i + 2] == 'x') ? strtoul(s + i + 3, nullptr, 16) : strtoul(s + i + 2, nullptr, 10);
    if (!cp) {
      out[o++] = s[i++];
      continue;
    }
    if (cp < 0x80) out[o++] = (char)cp;
    else if (cp < 0x800) { out[o++] = 0xC0 | (cp >> 6); out[o++] = 0x80 | (cp & 0x3F); }
    else if (cp < 0x10000) { out[o++] = 0xE0 | (cp >> 12); out[o++] = 0x80 | ((cp >> 6) & 0x3F); out[o++] = 0x80 | (cp & 0x3F); }
    else if (cp <= 0x10FFFF) {
      out[o++] = 0xF0 | (cp >> 18);
      out[o++] = 0x80 | ((cp >> 12) & 0x3F);
      out[o++] = 0x80 | ((cp >> 6) & 0x3F);
      out[o++] = 0x80 | (cp & 0x3F);
    }
    i += el;
  }
  out[o] = 0;
}

static const char* findTag(const char* xml, const char* name) {
  size_t nl = strlen(name);
  for (const char* p = xml; (p = strchr(p, '<')); p++) {
    if (!strncmp(p + 1, name, nl) && strchr(" \t\r\n/>", p[1 + nl])) return p;
  }
  return nullptr;
}

static bool tagAttr(const char* tag, const char* name, char* out, size_t out_len) {
  out[0] = 0;
  if (!tag) return false;
  const char* end = strchr(tag, '>');
  if (!end) return false;
  size_t nl = strlen(name);
  for (const char* p = tag + 1; p + nl + 2 < end; p++) {
    if (!strchr(" \t\r\n", p[-1]) || strncmp(p, name, nl) || p[nl] != '=') continue;
    char q = p[nl + 1];
    if (q != '"' && q != '\'') continue;
    const char* v = p + nl + 2;
    const char* e = strchr(v, q);
    if (!e) return false;
    xmlUnescape(v, e - v, out, out_len);
    return true;
  }
  return false;
}

void TakClient::rxConsume(TakInSock& s, size_t n){
  if (n > s.rx_len) n = s.rx_len;
  memmove(s.rx, s.rx + n, s.rx_len - n + 1);
  s.rx_len -= n;
}

void TakClient::noteType(const char* type, bool proto) {
  if (proto) inbound.proto++;
  else inbound.xml++;
  size_t i = 0;
  if (type) {
    for (; type[i] && i + 1 < sizeof(inbound.type); i++) {
      char c = type[i];
      inbound.type[i] = (c >= 32 && c < 127) ? c : '?';
    }
  }
  inbound.type[i] = 0;
  if (proto && inbound.proto == 1) Serial.println("[TAK] inbound framing is protobuf (0xbf)");
  if (!proto && inbound.xml == 1) Serial.printf("[TAK] inbound framing is XML, first type %s\n", inbound.type);
}

// TAK Protocol streaming header is 0xbf + an unsigned varint payload length
// (TAK Protocol spec, "Streaming Connections"). Version 1 payload is a TakMessage:
// field 2 = CotEvent, CotEvent field 1 = type. Return 1 = consumed, 0 = need more, -1 = resync.
static int pbVarint(const uint8_t*& p, const uint8_t* end, uint64_t& v) {
  v = 0;
  int shift = 0;
  while (shift <= 63) {
    if (p >= end) return 0;
    uint8_t b = *p++;
    v |= (uint64_t)(b & 0x7f) << shift;
    if (!(b & 0x80)) return 1;
    shift += 7;
  }
  return -1;
}

static bool pbField(const uint8_t* start, const uint8_t* end, uint32_t field, const uint8_t*& out, size_t& out_len) {
  const uint8_t* p = start;
  while (p < end) {
    uint64_t key = 0;
    if (pbVarint(p, end, key) != 1) return false;
    uint32_t fn = (uint32_t)(key >> 3);
    uint32_t wt = (uint32_t)(key & 7);
    if (wt == 2) {
      uint64_t n = 0;
      if (pbVarint(p, end, n) != 1 || n > (uint64_t)(end - p)) return false;
      if (fn == field) {
        out = p;
        out_len = (size_t)n;
        return true;
      }
      p += (size_t)n;
    } else if (wt == 0) {
      uint64_t skip = 0;
      if (pbVarint(p, end, skip) != 1) return false;
    } else if (wt == 1) {
      if ((size_t)(end - p) < 8) return false;
      p += 8;
    } else if (wt == 5) {
      if ((size_t)(end - p) < 4) return false;
      p += 4;
    } else {
      return false;
    }
  }
  return false;
}

static bool pbString(const uint8_t* start, const uint8_t* end, uint32_t field, char* dest, size_t dest_len) {
  const uint8_t* s = nullptr;
  size_t n = 0;
  if (!pbField(start, end, field, s, n) || !dest_len) return false;
  if (n >= dest_len) n = dest_len - 1;
  memcpy(dest, s, n);
  dest[n] = 0;
  return true;
}

static bool typeLooksCot(const char* type) {
  if (!type || strlen(type) < 3 || !strchr(type, '-')) return false;
  for (const char* c = type; *c; c++) {
    if (*c < 32 || *c > 126) return false;
  }
  return true;
}

// How far a raw TakMessage extends. 1 = one message, consumed set. 0 = need more
// bytes. -1 = not a protobuf message. A 0xbf at a field boundary is the next frame
// (wire type 7 is illegal, so that byte cannot be a field key).
static int pbMeasure(const uint8_t* start, const uint8_t* end, size_t& consumed) {
  const uint8_t* p = start;
  if (p >= end) return 0;
  while (p < end) {
    if (*p == 0xbf && p != start) {
      consumed = (size_t)(p - start);
      return 1;
    }
    uint64_t key = 0;
    int rc = pbVarint(p, end, key);
    if (rc == 0) return 0;
    if (rc < 0) return -1;
    uint32_t wt = (uint32_t)(key & 7);
    if (wt == 2) {
      uint64_t n = 0;
      rc = pbVarint(p, end, n);
      if (rc == 0) return 0;
      if (rc < 0) return -1;
      if (n > (uint64_t)(end - p)) return 0;
      p += (size_t)n;
    } else if (wt == 0) {
      uint64_t skip = 0;
      rc = pbVarint(p, end, skip);
      if (rc == 0) return 0;
      if (rc < 0) return -1;
    } else if (wt == 1) {
      if ((size_t)(end - p) < 8) return 0;
      p += 8;
    } else if (wt == 5) {
      if ((size_t)(end - p) < 4) return 0;
      p += 4;
    } else {
      return -1;
    }
  }
  return 0;
}

void TakClient::processRx(TakInSock& s){
  if (!s.logged_head && s.rx_len >= 4) {
    s.logged_head = true;
    Serial.printf("[TAK] rx head %02x %02x %02x %02x\n", (uint8_t)s.rx[0], (uint8_t)s.rx[1], (uint8_t)s.rx[2],
                  (uint8_t)s.rx[3]);
  }
  if (s.proto_skip) {
    size_t n = s.proto_skip < s.rx_len ? s.proto_skip : s.rx_len;
    rxConsume(s, n);
    s.proto_skip -= n;
    if (s.proto_skip) return;
  }

  while (s.rx_len) {
    if ((uint8_t)s.rx[0] == ' ' || s.rx[0] == '\t' || s.rx[0] == '\r' || s.rx[0] == '\n') {
      rxConsume(s, 1);
      continue;
    }
    if ((uint8_t)s.rx[0] == 0xbf) {
      const uint8_t* buf = (const uint8_t*)s.rx;
      const uint8_t* end = buf + s.rx_len;
      const uint8_t* msg = nullptr;
      size_t msg_len = 0;
      size_t frame = 0;
      // Multicast header is 0xbf 0x01 0xbf plus a raw TakMessage. A stream frame
      // whose length varint is 1 is the only lookalike, and a CotEvent is longer.
      bool mesh = s.rx_len >= 4 && buf[1] == 0x01 && buf[2] == 0xbf && ((buf[3] & 7) == 0 || (buf[3] & 7) == 1 ||
                                                                        (buf[3] & 7) == 2 || (buf[3] & 7) == 5);
      if (mesh) {
        size_t consumed = 0;
        int m = pbMeasure(buf + 3, end, consumed);
        if (m == 0) {
          if (s.rx_len < sizeof(s.rx) - 1) return;
          Serial.println("[TAK] mesh protobuf larger than buffer, resync");
          rxConsume(s, 1);
          continue;
        }
        if (m < 0) {
          rxConsume(s, 1);
          continue;
        }
        msg = buf + 3;
        msg_len = consumed;
        frame = 3 + consumed;
      } else {
        const uint8_t* after = buf + 1;
        uint64_t len = 0;
        int rc = pbVarint(after, end, len);
        if (rc == 0) return;
        if (rc < 0) {
          rxConsume(s, 1);
          continue;
        }
        size_t head = (size_t)(after - buf);
        if (len > 3500) {
          s.proto_skip = (size_t)len;
          rxConsume(s, head);
          size_t n = s.proto_skip < s.rx_len ? s.proto_skip : s.rx_len;
          rxConsume(s, n);
          s.proto_skip -= n;
          Serial.printf("[TAK] skipped oversized protobuf (%u bytes)\n", (unsigned)len);
          if (s.proto_skip) return;
          continue;
        }
        if (s.rx_len < head + (size_t)len) return;
        msg = buf + head;
        msg_len = (size_t)len;
        frame = head + msg_len;
      }

      const uint8_t* cot = nullptr;
      size_t cot_len = 0;
      char type[40] = {0};
      bool got = pbField(msg, msg + msg_len, 2, cot, cot_len) && pbString(cot, cot + cot_len, 1, type, sizeof(type)) &&
                 typeLooksCot(type);
      if (!got) got = pbString(msg, msg + msg_len, 1, type, sizeof(type)) && typeLooksCot(type);
      // Server traffic is only counted; the gateway publishes and never relays TAK events to the mesh.
      noteType(got ? type : "", true);
      rxConsume(s, frame);
      continue;
    }

    char* endtag = strstr(s.rx, "</event>");
    if (!endtag) {
      if (s.rx_len >= sizeof(s.rx) - 1) {
        char* next = strstr(s.rx + 1, "<event");
        void* bf = memchr(s.rx + 1, 0xbf, s.rx_len - 1);
        char* keep = next;
        if (bf && (!keep || (char*)bf < keep)) keep = (char*)bf;
        if (!keep) {
          s.rx_len = 0;
          s.rx[0] = 0;
        } else {
          rxConsume(s, (size_t)(keep - s.rx));
        }
        Serial.println("[TAK] rx resync (event larger than buffer)");
        continue;
      }
      return;
    }
    endtag += 8;
    char keep = *endtag;
    *endtag = 0;
    handleEvent(s, s.rx);
    *endtag = keep;
    rxConsume(s, (size_t)(endtag - s.rx));
  }
}

// XML from the server is pings, protocol negotiation and whatever the certificate's groups
// carry. Only negotiation is acted on.
void TakClient::handleEvent(TakInSock& s, const char* ev) {
  const char* e = findTag(ev, "event");
  char type[40];
  if (!e || !tagAttr(e, "type", type, sizeof(type))) return;
  noteType(type, false);
  if (!strcmp(type, "t-x-takp-v")) onProtoOffer(s, e);
  else if (!strcmp(type, "t-x-takp-r")) onProtoAnswer(s, e);
}

const char* TakClient::roomFor(int ch) const {
  if (!_cfg) return "?";
  if (ch == TAK_PUBLIC_SLOT) return _cfg->prefs.public_room;
  return (ch >= 0 && ch < TAK_MAX_CHAT) ? _cfg->prefs.chat[ch].room : "?";
}

bool TakClient::chatEnabled() const {
  if (!_cfg) return false;
  if (_cfg->prefs.public_on) return true;
  for (int i = 0; i < TAK_MAX_CHAT; i++) {
    if (_cfg->prefs.chat[i].enabled && _cfg->prefs.chat[i].secret_len) return true;
  }
  return false;
}

void TakClient::noteChat(int ch, const char* sender, const char* text) {
  chat.mesh_to_tak++;
  chat.last_ms = millis();
  memmove(&chat.recent[1], &chat.recent[0], sizeof(TakChatMsg) * (TakChatStats::RECENT - 1));
  if (chat.recent_n < TakChatStats::RECENT) chat.recent_n++;
  TakChatMsg& m = chat.recent[0];
  m.ms = chat.last_ms;
  snprintf(m.text, sizeof(m.text), "[%s] %s: %s", roomFor(ch), sender, text);
  utf8TrimTail(m.text);
}

bool TakClient::queueChat(int ch, const char* sender, const char* text) {
  if (!_cfg || ch < 0 || ch > TAK_PUBLIC_SLOT) return false;
  time_t now = nowUtc();
  if (!now) return false;
  size_t n = TakCot::buildChat(_tx_buf, sizeof(_tx_buf), _gw_uid, roomFor(ch), sender, text,
                               _cfg->prefs.chat_lat, _cfg->prefs.chat_lon, now);
  return n && enqueueXml(_tx_buf, n, TakEvKind::Chat);
}

bool TakClient::enqueueXml(const char* xml, size_t len, TakEvKind kind, const char* name, const char* role) {
  if (!xml || len == 0 || len >= Q_ITEM_MAX) return false;
  // Callers retry a refused event, which beats starving the TLS sessions and Wi-Fi of heap.
  if (ESP.getFreeHeap() < Q_MIN_HEAP + len) return false;
  char* copy = (char*)malloc(len + 1);
  if (!copy) return false;
  if (_q_count >= QSIZE) {
    free(_q[_q_head]);
    _q[_q_head] = nullptr;
    _q_head = (_q_head + 1) % QSIZE;
    _q_count--;
    link.dropped++;
  }
  memcpy(copy, xml, len);
  copy[len] = 0;
  _q[_q_tail] = copy;
  _q_kind[_q_tail] = kind;
  utf8Copy(_q_name[_q_tail], sizeof(_q_name[0]), name);
  utf8Copy(_q_role[_q_tail], sizeof(_q_role[0]), role);
  _q_tail = (_q_tail + 1) % QSIZE;
  _q_count++;
  return true;
}

bool TakClient::dequeueXml(char* dest, size_t dest_len, size_t& out_len, TakEvKind& kind, char* name,
                           size_t name_len, char* role, size_t role_len) {
  if (_q_count == 0) return false;
  size_t len = strlen(_q[_q_head]);
  if (len + 1 > dest_len) return false;
  memcpy(dest, _q[_q_head], len + 1);
  free(_q[_q_head]);
  _q[_q_head] = nullptr;
  out_len = len;
  kind = _q_kind[_q_head];
  utf8Copy(name, name_len, _q_name[_q_head]);
  utf8Copy(role, role_len, _q_role[_q_head]);
  _q_head = (_q_head + 1) % QSIZE;
  _q_count--;
  return true;
}

struct PbBuf {
  uint8_t* b;
  size_t n;
  size_t cap;
  bool ok;
  void varint(uint64_t v) {
    if (!ok) return;
    uint8_t tmp[10];
    int i = 0;
    do {
      uint8_t byte = (uint8_t)(v & 0x7f);
      v >>= 7;
      if (v) byte |= 0x80;
      tmp[i++] = byte;
    } while (v && i < 10);
    if (v || n + (size_t)i > cap) {
      ok = false;
      return;
    }
    memcpy(b + n, tmp, (size_t)i);
    n += (size_t)i;
  }
  void key(uint32_t field, uint32_t wt) { varint(((uint64_t)field << 3) | wt); }
  void bytes(uint32_t field, const void* s, size_t len) {
    key(field, 2);
    varint(len);
    if (!ok || n + len > cap) {
      ok = false;
      return;
    }
    if (len) memcpy(b + n, s, len);
    n += len;
  }
  void str(uint32_t field, const char* s) { bytes(field, s, s ? strlen(s) : 0); }
  void u64(uint32_t field, uint64_t v) {
    key(field, 0);
    varint(v);
  }
  void f64(uint32_t field, double v) {
    key(field, 1);
    if (!ok || n + 8 > cap) {
      ok = false;
      return;
    }
    memcpy(b + n, &v, 8);  // ESP32 is little-endian, which is the protobuf fixed64 layout
    n += 8;
  }
  void msg(uint32_t field, const PbBuf& m) {
    if (!m.ok) ok = false;
    bytes(field, m.b, m.n);
  }
};

static uint64_t cotTimeMs(const char* s) {
  int Y, M, D, h, m, sec;
  if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &sec) != 6) return 0;
  int ms = 0;
  const char* dot = strchr(s, '.');
  if (dot) ms = atoi(dot + 1);
  // Days from civil date (Howard Hinnant), then UTC seconds. newlib has no timegm here.
  int y = Y;
  unsigned mo = (unsigned)M;
  y -= mo <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + (unsigned)D - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  int64_t days = (int64_t)era * 146097 + (int64_t)doe - 719468;
  int64_t sec_utc = days * 86400 + h * 3600 + m * 60 + sec;
  if (sec_utc < 0) return 0;
  return (uint64_t)sec_utc * 1000ull + (uint64_t)ms;
}

static void fillStr(PbBuf& m, uint32_t field, const char* tag, const char* attr) {
  char tmp[96];
  if (tag && tagAttr(tag, attr, tmp, sizeof(tmp)) && tmp[0]) m.str(field, tmp);
}

// TAK Protocol stream frame: 0xbf, varint length, one TakMessage.
// CotEvent.detail.xmlDetail keeps the detail XML; contact, __group and takv are also
// the typed fields from takproto's detail.proto so a protobuf client still sees them.
static size_t cotToStream(const char* xml, uint8_t* dest, size_t cap) {
  const char* ev = findTag(xml, "event");
  if (!ev) return 0;
  char type[40], uid[80], how[24], ts[40], tstart[40], tstale[40];
  if (!tagAttr(ev, "type", type, sizeof(type)) || !type[0]) return 0;
  tagAttr(ev, "uid", uid, sizeof(uid));
  tagAttr(ev, "how", how, sizeof(how));
  tagAttr(ev, "time", ts, sizeof(ts));
  tagAttr(ev, "start", tstart, sizeof(tstart));
  tagAttr(ev, "stale", tstale, sizeof(tstale));
  if (!how[0]) snprintf(how, sizeof(how), "m-g");
  uint64_t send_ms = cotTimeMs(ts);
  uint64_t start_ms = cotTimeMs(tstart[0] ? tstart : ts);
  uint64_t stale_ms = cotTimeMs(tstale[0] ? tstale : ts);
  if (!send_ms) return 0;
  if (!start_ms) start_ms = send_ms;
  if (!stale_ms) stale_ms = send_ms;

  const char* pt = findTag(ev, "point");
  char num[32];
  auto fattr = [&](const char* attr, double fallback) {
    if (pt && tagAttr(pt, attr, num, sizeof(num)) && num[0]) return atof(num);
    return fallback;
  };
  double lat = fattr("lat", 0);
  double lon = fattr("lon", 0);
  double hae = fattr("hae", 9999999.0);
  double ce = fattr("ce", 9999999.0);
  double le = fattr("le", 9999999.0);

  const char* det = findTag(ev, "detail");
  const char* open = det ? strchr(det, '>') : nullptr;
  const char* close = open ? strstr(open, "</detail>") : nullptr;
  const char* inner = (open && close && open[1] != '/') ? open + 1 : nullptr;
  size_t inner_len = inner ? (size_t)(close - inner) : 0;

  PbBuf detail{s_detail, 0, sizeof(s_detail), true};
  if (inner_len) detail.bytes(1, inner, inner_len > 1800 ? 1800 : inner_len);
  const char* contact = det ? findTag(det, "contact") : nullptr;
  const char* group = det ? findTag(det, "__group") : nullptr;
  const char* takv = det ? findTag(det, "takv") : nullptr;
  uint8_t sub[192];
  if (contact) {
    PbBuf c{sub, 0, sizeof(sub), true};
    fillStr(c, 1, contact, "endpoint");
    fillStr(c, 2, contact, "callsign");
    if (c.ok && c.n) detail.msg(2, c);
  }
  if (group) {
    PbBuf g{sub, 0, sizeof(sub), true};
    fillStr(g, 1, group, "name");
    fillStr(g, 2, group, "role");
    if (g.ok && g.n) detail.msg(3, g);
  }
  if (takv) {
    PbBuf v{sub, 0, sizeof(sub), true};
    fillStr(v, 1, takv, "device");
    fillStr(v, 2, takv, "platform");
    fillStr(v, 3, takv, "os");
    fillStr(v, 4, takv, "version");
    if (v.ok && v.n) detail.msg(6, v);
  }
  if (!detail.ok) return 0;

  PbBuf cot{s_cot, 0, sizeof(s_cot), true};
  cot.str(1, type);
  if (uid[0]) cot.str(5, uid);
  cot.u64(6, send_ms);
  cot.u64(7, start_ms);
  cot.u64(8, stale_ms);
  cot.str(9, how);
  cot.f64(10, lat);
  cot.f64(11, lon);
  cot.f64(12, hae);
  cot.f64(13, ce);
  cot.f64(14, le);
  if (detail.n) cot.msg(15, detail);
  if (!cot.ok) return 0;

  // TakMessage is just field 2 (cotEvent), so it is written straight into the frame.
  auto varLen = [](uint64_t v) {
    size_t n = 1;
    while (v >>= 7) n++;
    return n;
  };
  auto putVar = [&](size_t& i, uint64_t v) {
    do {
      uint8_t byte = (uint8_t)(v & 0x7f);
      v >>= 7;
      if (v) byte |= 0x80;
      dest[i++] = byte;
    } while (v);
  };
  size_t tak_len = 1 + varLen(cot.n) + cot.n;
  if (tak_len + 12 > cap) return 0;
  size_t i = 0;
  dest[i++] = 0xbf;
  putVar(i, tak_len);
  dest[i++] = (2 << 3) | 2;
  putVar(i, cot.n);
  memcpy(dest + i, cot.b, cot.n);
  return i + cot.n;
}

void TakClient::onProtoOffer(TakInSock& s, const char* ev){
  if (s.out_proto == 2 || s.want_proto) return;
  bool v1 = false;
  for (const char* p = ev; (p = findTag(p, "TakProtocolSupport")); p++) {
    char ver[8];
    if (tagAttr(p, "version", ver, sizeof(ver)) && !strcmp(ver, "1")) v1 = true;
  }
  if (!v1) {
    snprintf(inbound.note, sizeof(inbound.note), "server protocol offer has no version 1");
    Serial.println("[TAK] protocol offer without version 1, staying on XML");
    return;
  }
  tagAttr(ev, "uid", s.proto_uid, sizeof(s.proto_uid));
  if (!s.proto_uid[0]) {
    snprintf(inbound.note, sizeof(inbound.note), "protocol offer missing uid");
    return;
  }
  s.want_proto = true;
  snprintf(inbound.note, sizeof(inbound.note), "server offered TAK protocol 1");
  Serial.println("[TAK] server offered TAK protocol 1");
}

void TakClient::onProtoAnswer(TakInSock& s, const char* ev){
  if (s.out_proto != 1) return;
  char status[8] = {0};
  tagAttr(findTag(ev, "TakResponse"), "status", status, sizeof(status));
  if (!strcasecmp(status, "true")) {
    s.out_proto = 2;
    s.want_proto = false;
    snprintf(inbound.note, sizeof(inbound.note), "protobuf streaming on");
    Serial.println("[TAK] protobuf streaming accepted");
  } else {
    s.out_proto = 0;
    s.want_proto = false;
    snprintf(inbound.note, sizeof(inbound.note), "protobuf streaming denied");
    Serial.printf("[TAK] protobuf streaming denied (%s)\n", status[0] ? status : "no status");
  }
}

int TakClient::writeCot(const char* xml, size_t len) {
  if (!xml || !_tls || !len) return -1;
  const void* data = xml;
  size_t n = len;
  if (_pub.out_proto == 2) {
    n = cotToStream(xml, s_out, sizeof(s_out));
    if (!n) {
      Serial.println("[TAK] protobuf encode failed");
      return -1;
    }
    data = s_out;
  } else if (len + 2 < sizeof(s_out)) {
    char* lined = (char*)s_out;
    memcpy(lined, xml, len);
    lined[len] = 0;
    char* hdr = strstr(lined, "?>");
    if (hdr && hdr[2] != '\n') {
      size_t at = (size_t)(hdr + 2 - lined);
      memmove(lined + at + 1, lined + at, len - at + 1);
      lined[at] = '\n';
      n = len + 1;
    }
    data = lined;
  }
  int w = esp_tls_conn_write(TLS, data, n);
  if (w < 0 || (size_t)w != n) return -1;
  return (int)n;
}

void TakClient::noteSent(TakEvKind kind, const char* name, const char* role) {
  link.events++;
  link.last_tx_ms = millis();
  if (kind == TakEvKind::Point) {
    link.points++;
    link.last_point_ms = link.last_tx_ms;
    utf8Copy(link.last_point, sizeof(link.last_point), name);
  } else if (kind == TakEvKind::Tracker) {
    link.trackers++;
    link.last_tracker_ms = link.last_tx_ms;
    utf8Copy(link.last_tracker, sizeof(link.last_tracker), name);
    const char* key = (role && role[0]) ? role : "-";
    int i = 0;
    while (i < link.roles_n && strcasecmp(link.roles[i].key, key) != 0) i++;
    if (i < link.roles_n) {
      link.roles[i].n++;
    } else if (link.roles_n < TakLinkStats::ROLE_SLOTS) {
      utf8Copy(link.roles[i].key, sizeof(link.roles[i].key), key);
      link.roles[i].n = 1;
      link.roles_n++;
    } else {
      link.roles_other++;
    }
  } else if (kind == TakEvKind::Delete) {
    link.removed++;
  } else if (kind == TakEvKind::Chat) {
    link.chats++;
  }
}

bool TakClient::queuePoint(const TakNodeRecord& node) {
  if (!_cfg) return false;
  if (!TakNodes::passesFilter(node.name, _cfg->prefs)) return false;
  time_t now = nowUtc();
  if (!now) {
    setError("NTP not ready");
    return false;
  }
  size_t n = TakCot::buildPoint(_tx_buf, sizeof(_tx_buf), node, _cfg->prefs, now);
  if (!n) return false;
  return enqueueXml(_tx_buf, n, TakEvKind::Point, node.name);
}

bool TakClient::queueTrackerPoint(const TakTrackerRecord& rec, const TakCotStyle& style) {
  if (!_cfg || !_cfg->prefs.enabled || !rec.cot_time) return false;
  size_t n = TakCot::buildTrackerPoint(_tx_buf, sizeof(_tx_buf), rec.uid, rec.callsign, rec.lat, rec.lon,
                                       rec.has_altitude, rec.altitude_m, rec.has_speed, rec.speed_mps, rec.has_course,
                                       rec.course_deg, style, rec.cot_time, rec.stale_sec);
  if (!n) return false;
  return enqueueXml(_tx_buf, n, TakEvKind::Tracker, rec.callsign, rec.role);
}

bool TakClient::queuePing() {
  time_t now = nowUtc();
  if (!now) return false;
  char t[32], st[32];
  struct tm tm_now;
  gmtime_r(&now, &tm_now);
  strftime(t, sizeof(t), "%Y-%m-%dT%H:%M:%SZ", &tm_now);
  time_t stale = now + 60;
  gmtime_r(&stale, &tm_now);
  strftime(st, sizeof(st), "%Y-%m-%dT%H:%M:%SZ", &tm_now);
  int n = snprintf(_tx_buf, sizeof(_tx_buf),
                   "<?xml version=\"1.0\" encoding=\"UTF-8\"?><event version=\"2.0\" uid=\"takPing\" "
                   "type=\"t-x-c-t\" how=\"h-g-i-g-o\" time=\"%s\" start=\"%s\" stale=\"%s\">"
                   "<point lat=\"0\" lon=\"0\" hae=\"0\" ce=\"9999999\" le=\"9999999\"/><detail/></event>",
                   t, t, st);
  if (n <= 0 || (size_t)n >= sizeof(_tx_buf)) return false;
  return enqueueXml(_tx_buf, n);
}

bool TakClient::queueDelete(const char* uid) {
  time_t now = nowUtc();
  if (!now || !uid) return false;
  size_t n = TakCot::buildDelete(_tx_buf, sizeof(_tx_buf), uid, now);
  if (!n) return false;
  return enqueueXml(_tx_buf, n, TakEvKind::Delete);
}

void TakClient::processRefreshExpire() {
  if (!_nodes || !_cfg) return;
  TakNodeRecord* refresh[TAK_MAX_NODES];
  TakNodeRecord* expire[TAK_MAX_NODES];
  int nr = 0, ne = 0;
  _nodes->collectRefreshAndExpire(millis(), _cfg->prefs.refresh_sec, _cfg->prefs.max_age_sec,
                                  refresh, TAK_MAX_NODES, nr, expire, TAK_MAX_NODES, ne);
  for (int i = 0; i < nr; i++) {
    if (!TakNodes::passesFilter(refresh[i]->name, _cfg->prefs)) continue;
    if (queuePoint(*refresh[i])) _nodes->markSent(refresh[i], millis());
  }
  for (int i = 0; i < ne; i++) {
    if (expire[i]->last_sent_ms) queueDelete(expire[i]->uid);
    expire[i]->valid = false;
  }
}

void TakClient::removeFiltered() {
  if (!_nodes || !_cfg) return;
  for (int i = 0; i < _nodes->count(); i++) {
    TakNodeRecord* n = _nodes->at(i);
    if (n && n->last_sent_ms && !TakNodes::passesFilter(n->name, _cfg->prefs) && queueDelete(n->uid)) {
      n->last_sent_ms = 0;
      Serial.printf("[TAK] removed (name filter): %s\n", n->name);
    }
  }
}

void TakClient::bumpBackoff() {
  setState(TakLinkState::Backoff);
  _backoff_until = millis() + _backoff_ms;
  if (_backoff_ms < 60000) _backoff_ms = min(_backoff_ms * 2, (uint32_t)60000);
  disconnectTls();
}

bool TakClient::wantsNetwork() const {
  return _cfg && _cfg->prefs.enabled && !_paused && _cfg->hasTakHost() && _cfg->hasClientCerts();
}

const char* TakClient::reconnectBlocker() const {
  if (!_cfg) return "not ready";
  if (!_cfg->prefs.enabled) return "Turn on Send to TAK Server and save first";
  if (!_cfg->hasWifi()) return "Wi-Fi is not set up";
  if (!_cfg->hasTakHost()) return "TAK host is not set";
  if (!_cfg->hasClientCerts()) return "Install the certificate first";
  return nullptr;
}

void TakClient::reconnect() {
  disconnectTls();
  setError("");
  requestConnect();
}

void TakClient::loop() {
  if (!_cfg) return;

  if (!_cfg->prefs.enabled || _paused) {
    if (_state != TakLinkState::Disabled) {
      disconnectTls();
      setState(TakLinkState::Disabled);
    }
    return;
  }
  if (_state == TakLinkState::Disabled) setState(TakLinkState::WaitWifi);  // enabled again or unpaused

  if (_state == TakLinkState::Backoff) {
    if ((long)(millis() - _backoff_until) < 0) return;
    setState(TakLinkState::WaitWifi);
  }

  if (_state == TakLinkState::WaitWifi || _state == TakLinkState::Error) {
    if (WiFi.status() == WL_CONNECTED) {
      setState(TakLinkState::WaitNtp);
      configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    }
    return;
  }

  if (_state == TakLinkState::WaitNtp) {
    if (nowUtc()) {
      _ntp_ok = true;
      if (!connectTls()) bumpBackoff();
    }
    return;
  }

  if (_state == TakLinkState::Connecting) return;

  if (_state == TakLinkState::Connected) {
    if (!_tls) {
      setError("connection lost");
      bumpBackoff();
      return;
    }
    if (WiFi.status() != WL_CONNECTED) {
      setError("Wi-Fi lost - reconnecting");
      Serial.println("[TAK] Wi-Fi lost, dropping TLS");
      bumpBackoff();
      return;
    }
    _net_ok_ms = millis();
    if ((long)(millis() - _next_drain) >= 0) {
      _next_drain = millis() + 50;
      if (!drainInbound()) {
        captureTlsError("server closed connection");
        bumpBackoff();
        return;
      }
    }
    sendProtoAsk(_tls, _pub, "publish");
    if (_pub.out_proto == 1 && millis() - _pub.proto_wait_ms > 60000UL) {
      Serial.println("[TAK] protocol negotiation timed out");
      setError("protocol negotiation timed out");
      bumpBackoff();
      return;
    }
    if (millis() - _last_ping_ms >= 60000UL) {
      _last_ping_ms = millis();
      if (_pub.out_proto != 1) queuePing();
    }
    if ((long)(millis() - _next_refresh) >= 0) {
      processRefreshExpire();
      tak_trackers.flush(*this);
      _next_refresh = millis() + 1000;
    }
    // Hold CoT while the server decides the protocol. After it accepts, both
    // directions are length-framed protobuf (TAK streaming negotiation, step 7a).
    size_t len = 0;
    TakEvKind kind;
    char name[32];
    char role[TAK_TRACKER_ROLE_LEN];
    if (_pub.out_proto != 1 &&
        dequeueXml(_tx_buf, sizeof(_tx_buf), len, kind, name, sizeof(name), role, sizeof(role))) {
      int w = writeCot(_tx_buf, len);
      if (w < 0) {
        captureTlsError("write failed");
        bumpBackoff();
      } else {
        noteSent(kind, name, role);
      }
    }
  }
}
