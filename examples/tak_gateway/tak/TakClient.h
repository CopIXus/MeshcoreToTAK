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

enum class TakEvKind : uint8_t { Other, Point, Delete, Chat };

// Counts are of events actually written to the TAK server, not just queued.
struct TakLinkStats {
  uint32_t points = 0;    // marker updates
  uint32_t removed = 0;   // marker deletes
  uint32_t chats = 0;     // GeoChat messages (mesh -> TAK)
  uint32_t events = 0;    // everything, incl. keepalives and the gateway contact
  uint32_t connects = 0;  // TLS sessions opened since boot
  uint32_t dropped = 0;   // events lost because the send queue was full
  unsigned long up_since_ms = 0;
  unsigned long last_tx_ms = 0;
  unsigned long last_rx_ms = 0;
  unsigned long last_point_ms = 0;
  char last_point[32] = {0};
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
  void removeFiltered();  // delete map markers for nodes the name filter now rejects

  // MeshCore channel message -> GeoChat in that channel's TAK room
  bool queueChat(int ch, const char* sender, const char* text);
  // GeoChat from TAK waiting to go out on a MeshCore channel
  bool popChatIn(TakChatIn& out);
  void noteChat(bool from_mesh, int ch, const char* sender, const char* text);
  bool chatEnabled() const;
  const char* roomFor(int ch) const;
  void announce() { _presence_due = true; }
  // Drops the TAK link (from loop()) and keeps it down, e.g. to free heap for a firmware update.
  void pause(bool p) { _paused = p; }
  bool idle() const { return _tls == nullptr; }
  const char* gatewayUid() const { return _gw_uid; }
  TakChatStats chat;
  TakLinkStats link;
  int queued() const { return _q_count; }

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
  volatile bool _paused = false;
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
  TakEvKind _q_kind[QSIZE];
  char _q_name[QSIZE][32];
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
  bool enqueueXml(const char* xml, size_t len, TakEvKind kind = TakEvKind::Other, const char* name = nullptr);
  bool dequeueXml(char* dest, size_t dest_len, size_t& out_len, TakEvKind& kind, char* name, size_t name_len);
  void noteSent(TakEvKind kind, const char* name);
  void processRefreshExpire();
  void bumpBackoff();
  void captureTlsError(const char* prefix);
};
