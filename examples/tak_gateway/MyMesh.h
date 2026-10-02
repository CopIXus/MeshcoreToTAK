#pragma once

#include <Arduino.h>
#include <Mesh.h>
#include <helpers/ArduinoHelpers.h>
#include <helpers/StaticPoolPacketManager.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/IdentityStore.h>
#include <helpers/BaseChatMesh.h>
#include <target.h>

#include "tak/TakConfig.h"
#include "tak/TakNodes.h"
#include "tak/TakClient.h"

#ifndef LORA_FREQ
#define LORA_FREQ 910.525
#endif
#ifndef LORA_BW
#define LORA_BW 62.5
#endif
#ifndef LORA_SF
#define LORA_SF 7
#endif
#ifndef LORA_CR
#define LORA_CR 5
#endif
#ifndef LORA_TX_POWER
#define LORA_TX_POWER 22
#endif

#define SEND_TIMEOUT_BASE_MILLIS 500
#define FLOOD_SEND_TIMEOUT_FACTOR 16.0f
#define DIRECT_SEND_PERHOP_FACTOR 6.0f
#define DIRECT_SEND_PERHOP_EXTRA_MILLIS 250

class MyMesh : public BaseChatMesh {
  TakConfig* _cfg;
  TakNodes* _nodes;
  TakClient* _client;
  uint32_t expected_ack_crc = 0;

protected:
  float getAirtimeBudgetFactor() const override { return 2.0f; }  // listen-heavy
  int calcRxDelay(float score, uint32_t air_time) const override { return 0; }
  bool allowPacketForward(const mesh::Packet* packet) override { return false; }
  // SX1262 receivers can go deaf after hours of uptime; a periodic AGC reset keeps them listening.
  int getAGCResetInterval() const override { return 60000; }

  void logRx(mesh::Packet* packet, int len, float score) override;
  void onDiscoveredContact(ContactInfo& contact, bool is_new, uint8_t path_len, const uint8_t* path) override;
  void onContactPathUpdated(const ContactInfo& contact) override {}
  ContactInfo* processAck(const uint8_t* data) override {
    (void)data;
    return nullptr;
  }
  void onMessageRecv(const ContactInfo&, mesh::Packet*, uint32_t, const char*) override {}
  void onCommandDataRecv(const ContactInfo&, mesh::Packet*, uint32_t, const char*) override {}
  void onSignedMessageRecv(const ContactInfo&, mesh::Packet*, uint32_t, const uint8_t*, const char*) override {}
  void onChannelMessageRecv(const mesh::GroupChannel& channel, mesh::Packet* pkt, uint32_t timestamp,
                            const char* text) override;
  void onChannelDataRecv(const mesh::GroupChannel&, mesh::Packet*, uint16_t, const uint8_t*, size_t) override {}
  uint8_t onContactRequest(const ContactInfo&, uint32_t, const uint8_t*, uint8_t, uint8_t*) override { return 0; }
  void onContactResponse(const ContactInfo&, const uint8_t*, uint8_t) override {}
  uint32_t calcFloodTimeoutMillisFor(uint32_t pkt_airtime_millis) const override {
    return SEND_TIMEOUT_BASE_MILLIS + (uint32_t)(FLOOD_SEND_TIMEOUT_FACTOR * pkt_airtime_millis);
  }
  uint32_t calcDirectTimeoutMillisFor(uint32_t pkt_airtime_millis, uint8_t path_len) const override {
    uint8_t path_hash_count = path_len & 63;
    return SEND_TIMEOUT_BASE_MILLIS +
           ((uint32_t)(pkt_airtime_millis * DIRECT_SEND_PERHOP_FACTOR) + DIRECT_SEND_PERHOP_EXTRA_MILLIS) *
               (path_hash_count + 1);
  }
  void onSendTimeout() override {}

public:
  MyMesh(mesh::Radio& radio, StdRNG& rng, mesh::RTCClock& rtc, SimpleMeshTables& tables,
         TakConfig& cfg, TakNodes& nodes, TakClient& client)
      : BaseChatMesh(radio, *new ArduinoMillis(), rng, rtc, *new StaticPoolPacketManager(16), tables),
        _cfg(&cfg),
        _nodes(&nodes),
        _client(&client) {}

  void beginFs();
  void applyRadioFromConfig();
  void applyChannelsFromConfig();
  void requestAdvert() { _advert_due = true; }
  bool advertPending() const { return _advert_due; }
  unsigned long lastAdvertMs() const { return _last_advert_ms; }
  void loopGateway();

private:
  bool _advert_due = false;
  unsigned long _last_advert_ms = 0;
  bool sendSelfAdvert();
};
