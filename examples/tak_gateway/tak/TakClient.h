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

// What the TAK server actually sent us. chat_t2m only moves after a mesh send.
struct TakInTrace {
  uint32_t xml = 0;        // CoT events in XML
  uint32_t proto = 0;      // CoT events in TAK protocol protobuf (0xbf frames)
  uint32_t chat = 0;       // type b-t-f seen
  uint32_t chat_drop = 0;  // b-t-f not sent to the mesh
  char type[24] = {0};     // type of the latest event
  char note[96] = {0};     // latest GeoChat decision
};

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

// Optional second TLS session. It reads GeoChat and does not publish.
struct TakRxLink {
  TakInSock in;
  void* tls;
  TakLinkState state;
  char error[96];
  unsigned long backoff_until;
  uint32_t backoff_ms;
  unsigned long next_drain;
  unsigned long up_since_ms;
  unsigned long last_presence_ms = 0;
  bool presence_due = false;
  uint32_t events = 0;  // CoT events received, logged by type for the first few
  TakRxLink()
      : tls(nullptr),
        state(TakLinkState::Disabled),
        backoff_until(0),
        backoff_ms(1000),
        next_drain(0),
        up_since_ms(0) {
    error[0] = 0;
  }
};

class TakClient {
public:
  TakClient();
  ~TakClient();
  void begin(TakConfig* cfg, TakNodes* nodes);
  void loop();
  void requestConnect();
  // Drops both links and starts over without backoff. Main task only.
  void reconnect();
  const char* reconnectBlocker() const;  // why a reconnect cannot work, or nullptr
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
  bool idle() const { return _tls == nullptr && _rxlink.tls == nullptr; }
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
  const char* rxStateName() const;
  const char* rxError() const { return _rxlink.error; }
  long rxUpSeconds() const;
  void forgetReceive();

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
  static const uint32_t RX_MIN_HEAP_TO_OPEN = 70000;
  static const uint32_t RX_MIN_BLOCK_TO_OPEN = 24000;
  static const uint32_t RX_MIN_HEAP_RUNNING = 12000;
  static const uint32_t RX_LOW_HEAP_BACKOFF_MS = 120000;
  String _ca_pem;
  String _cert_pem;
  String _key_pem;

  char _tx_buf[2048];
  static const int QSIZE = 8;
  char* _q[QSIZE] = {};  // heap copies, sized to each event and freed once sent
  TakEvKind _q_kind[QSIZE];
  char _q_name[QSIZE][32];
  int _q_head = 0, _q_tail = 0, _q_count = 0;

  TakInSock _pub;
  TakRxLink _rxlink;
  TakInSock* _cur = nullptr;  // socket whose event handleEvent is matching
  static const int IN_SIZE = 4;
  TakChatIn _in[IN_SIZE];
  int _in_head = 0, _in_count = 0;
  char _gw_uid[32] = {0};
  bool _skip_cn = false;  // the server cert did not name "takserver"; check the CA chain only
  static const int TAK_SEEN_CHAT = 8;
  uint32_t _seen_chat[TAK_SEEN_CHAT] = {0};  // GeoChat messageId hashes already sent to the mesh
  int _seen_next = 0;
  bool _presence_due = false;
  unsigned long _last_presence_ms = 0;

  void processRx(TakInSock& s);
  void rxConsume(TakInSock& s, size_t n);
  void noteType(const char* type, bool proto);
  void handleEvent(const char* ev, bool proto);
  void onProtoOffer(TakInSock& s, const char* ev);
  void onProtoAnswer(TakInSock& s, const char* ev);
  int writeCot(const char* xml, size_t len) { return writeCotTo(_tls, _pub, xml, len); }
  int writeCotTo(void* tls, const TakInSock& s, const char* xml, size_t len);
  bool rxCarriesPresence() const;
  void noteLinkEvent(bool receive, const char* type, bool proto);
  uint32_t _pub_events = 0;
  bool sendProtoAsk(void* tls, TakInSock& s, const char* tag);
  bool openSession(void*& slot, const String& cert, const String& key, uint16_t port, const char* tag, char* err,
                   size_t err_len);
  bool connectRx();
  void disconnectRx();
  bool drainRx();
  void serviceRx();
  void bumpRx(const char* why);

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
