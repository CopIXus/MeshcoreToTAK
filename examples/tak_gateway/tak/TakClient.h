#pragma once

#include <Arduino.h>
#include "TakConfig.h"
#include "TakNodes.h"
#include "TakTrackers.h"

enum class TakLinkState : uint8_t {
  Disabled = 0,
  WaitWifi,
  WaitNtp,
  Connecting,
  Connected,
  Backoff,
  Error
};

struct TakChatMsg {
  unsigned long ms;
  char text[120];  // "[room] sender: text"
};

// MeshCore channel messages relayed to TAK. The gateway is one-way.
struct TakChatStats {
  static const int RECENT = 3;
  uint32_t mesh_to_tak = 0;
  TakChatMsg recent[RECENT] = {};  // newest first
  int recent_n = 0;
  unsigned long last_ms = 0;
  const char* last() const { return recent_n ? recent[0].text : ""; }
};

enum class TakEvKind : uint8_t { Other, Point, Delete, Chat, Tracker };

// Tracker fixes written to TAK, per role key. Roles past the table count as other.
struct TakRoleCount {
  char key[TAK_TRACKER_ROLE_LEN];
  uint32_t n;
};

// What the TAK server sent on the link: pings, protocol negotiation and group traffic.
struct TakInTrace {
  uint32_t xml = 0;     // CoT events in XML
  uint32_t proto = 0;   // CoT events in TAK protocol protobuf (0xbf frames)
  char type[24] = {0};  // type of the latest event
  char note[96] = {0};  // latest protocol negotiation step
};

// Counts are of events actually written to the TAK server, not just queued.
struct TakLinkStats {
  uint32_t points = 0;    // advert marker updates
  uint32_t trackers = 0;  // tracker fixes
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
  unsigned long last_tracker_ms = 0;
  char last_tracker[32] = {0};
  static const int ROLE_SLOTS = 8;
  TakRoleCount roles[ROLE_SLOTS] = {};
  int roles_n = 0;
  uint32_t roles_other = 0;
};

// Inbound buffer and TAK protocol negotiation for one streaming connection.
struct TakInSock {
  char rx[4096];
  size_t rx_len;
  size_t proto_skip;
  bool logged_head;
  uint8_t out_proto;  // 0 XML, 1 waiting for TakResponse, 2 protobuf
  bool want_proto;
  unsigned long proto_wait_ms;
  char proto_uid[80];
  TakInSock()
      : rx_len(0), proto_skip(0), logged_head(false), out_proto(0), want_proto(false), proto_wait_ms(0) {
    rx[0] = 0;
    proto_uid[0] = 0;
  }
  void clear() {
    rx_len = 0;
    proto_skip = 0;
    logged_head = false;
    out_proto = 0;
    want_proto = false;
    proto_wait_ms = 0;
    proto_uid[0] = 0;
    rx[0] = 0;
  }
};

class TakClient {
public:
  TakClient();
  ~TakClient();
  void begin(TakConfig* cfg, TakNodes* nodes);
  void loop();
  void requestConnect();
  // Drops the link and starts over without backoff. Main task only.
  void reconnect();
  const char* reconnectBlocker() const;  // why a reconnect cannot work, or nullptr
  bool queuePoint(const TakNodeRecord& node);
  bool queueTrackerPoint(const TakTrackerRecord& rec, const TakCotStyle& style);
  bool queueDelete(const char* uid);
  bool hasConfig() const { return _cfg != nullptr; }
  const TakPrefs& config() const { return _cfg->prefs; }
  void removeFiltered();  // delete map markers for nodes the name filter now rejects

  // MeshCore channel message -> GeoChat in that channel's TAK room
  bool queueChat(int ch, const char* sender, const char* text);
  void noteChat(int ch, const char* sender, const char* text);
  bool chatEnabled() const;
  const char* roomFor(int ch) const;
  // Drops the TAK link (from loop()) and keeps it down, e.g. to free heap for a firmware update.
  void pause(bool p) { _paused = p; }
  bool idle() const { return _tls == nullptr; }
  const char* gatewayUid() const { return _gw_uid; }
  TakChatStats chat;
  TakLinkStats link;
  TakInTrace inbound;
  int queued() const { return _q_count; }

  TakLinkState state() const { return _state; }
  const char* stateName() const;
  const char* lastError() const { return _last_error; }
  bool ntpOk() const { return _ntp_ok; }
  // Last time a TCP connection reached the TAK server or the publish link was up.
  unsigned long netOkMs() const { return _net_ok_ms; }
  bool wantsNetwork() const;
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
  unsigned long _net_ok_ms = 0;
  uint32_t _backoff_ms = 1000;

  // Only held while a handshake is being set up; the CA lives in the esp_tls global store.
  bool _ca_ok = false;
  String _ca_pem;
  String _cert_pem;
  String _key_pem;

  char _tx_buf[2048];
  static const int QSIZE = 8;
  char* _q[QSIZE] = {};  // heap copies, sized to each event and freed once sent
  TakEvKind _q_kind[QSIZE];
  char _q_name[QSIZE][32];
  char _q_role[QSIZE][TAK_TRACKER_ROLE_LEN];
  int _q_head = 0, _q_tail = 0, _q_count = 0;

  TakInSock _pub;
  char _gw_uid[32] = {0};
  bool _skip_cn = false;  // the server cert did not name "takserver"; check the CA chain only

  void processRx(TakInSock& s);
  void rxConsume(TakInSock& s, size_t n);
  void noteType(const char* type, bool proto);
  void handleEvent(TakInSock& s, const char* ev);
  void onProtoOffer(TakInSock& s, const char* ev);
  void onProtoAnswer(TakInSock& s, const char* ev);
  int writeCot(const char* xml, size_t len);
  bool sendProtoAsk(void* tls, TakInSock& s, const char* tag);
  bool openSession(void*& slot, const String& cert, const String& key, uint16_t port, const char* tag, char* err,
                   size_t err_len);

  void setError(const char* msg);
  void setState(TakLinkState s);
  bool loadCertPems();
  bool connectTls();
  void disconnectTls();
  bool drainInbound();
  bool queuePing();
  bool enqueueXml(const char* xml, size_t len, TakEvKind kind = TakEvKind::Other, const char* name = nullptr,
                  const char* role = nullptr);
  bool dequeueXml(char* dest, size_t dest_len, size_t& out_len, TakEvKind& kind, char* name, size_t name_len,
                  char* role, size_t role_len);
  void noteSent(TakEvKind kind, const char* name, const char* role);
  void processRefreshExpire();
  void bumpBackoff();
  void captureTlsError(const char* prefix);
};
