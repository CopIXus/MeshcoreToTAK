#pragma once

#include <Arduino.h>
#include "TakConfig.h"
#include "TakNodes.h"

enum class TakLinkState : uint8_t {
  Disabled = 0,
  WaitWifi,
  WaitNtp,
  Connecting,
  Connected,
  Backoff,
  Error
};

#define TAK_CHAT_TEXT_LEN 164

struct TakChatIn {
  uint8_t ch;
  char sender[TAK_CALLSIGN_LEN];
  char text[TAK_CHAT_TEXT_LEN];
};

struct TakChatMsg {
  bool from_mesh;
  unsigned long ms;
  char text[120];  // "[room] sender: text"
};

struct TakChatStats {
  static const int RECENT = 3;
  uint32_t mesh_to_tak = 0;
  uint32_t tak_to_mesh = 0;
  TakChatMsg recent[RECENT] = {};  // newest first
  int recent_n = 0;
  unsigned long last_ms = 0;
  const char* last() const { return recent_n ? recent[0].text : ""; }
};

class TakClient {
public:
  TakClient();
  ~TakClient();
  void begin(TakConfig* cfg, TakNodes* nodes);
  void loop();
  void requestConnect();
  bool testConnection(String& error_out);
  bool queuePoint(const TakNodeRecord& node);
  bool queueDelete(const char* uid);

  // MeshCore channel message -> GeoChat in that channel's TAK room
  bool queueChat(int ch, const char* sender, const char* text);
  // GeoChat from TAK waiting to go out on a MeshCore channel
  bool popChatIn(TakChatIn& out);
  void noteChat(bool from_mesh, int ch, const char* sender, const char* text);
  bool chatEnabled() const;
  const char* roomFor(int ch) const;
  void announce() { _presence_due = true; }
  const char* gatewayUid() const { return _gw_uid; }
  TakChatStats chat;

  TakLinkState state() const { return _state; }
  const char* stateName() const;
  const char* lastError() const { return _last_error; }
  bool ntpOk() const { return _ntp_ok; }
  time_t nowUtc() const;
  bool tlsConnected() const { return _tls != nullptr && _state == TakLinkState::Connected; }

private:
  TakConfig* _cfg = nullptr;
  TakNodes* _nodes = nullptr;
  void* _tls = nullptr;  // esp_tls_t*
  TakLinkState _state = TakLinkState::Disabled;
  char _last_error[128];
  bool _ntp_ok = false;
  unsigned long _backoff_until = 0;
  unsigned long _next_drain = 0;
  unsigned long _next_refresh = 0;
  unsigned long _last_ping_ms = 0;
  uint32_t _backoff_ms = 1000;

  // PEM buffers must stay alive for the life of the TLS session
  String _ca_pem;
  String _cert_pem;
  String _key_pem;

  char _tx_buf[2048];
  static const int QSIZE = 8;
  char _q[QSIZE][2048];
  int _q_head = 0, _q_tail = 0, _q_count = 0;

  char _rx[4096];
  size_t _rx_len = 0;
  static const int IN_SIZE = 4;
  TakChatIn _in[IN_SIZE];
  int _in_head = 0, _in_count = 0;
  char _gw_uid[32] = {0};
  bool _presence_due = false;
  unsigned long _last_presence_ms = 0;

  void processRx();
  void handleEvent(const char* ev);

  void setError(const char* msg);
  void setState(TakLinkState s);
  bool loadCertPems();
  bool connectTls();
  void disconnectTls();
  bool drainInbound();
  bool queuePing();
  bool enqueueXml(const char* xml, size_t len);
  bool dequeueXml(char* dest, size_t dest_len, size_t& out_len);
  void processRefreshExpire();
  void bumpBackoff();
  void captureTlsError(const char* prefix);
};
