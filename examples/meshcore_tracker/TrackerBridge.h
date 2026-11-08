#pragma once

#include <stdint.h>

class MyMesh;

struct TrackerScreenInfo {
  char role[5];
  char channel[16];
  char uid[9];
  char scope[32];
  uint8_t slot;
  bool keyed;
  bool has_fix;
  char problem[20];
  bool sent;
  double lat;
  double lon;
  uint32_t sequence;
  uint32_t age_s;
  uint32_t no_fix_s;
  uint32_t fixes_sent;
  uint32_t echoes;
  uint8_t echo_hops;
  uint32_t flood_heard;
};

namespace mesh {
class Packet;
}

// Companion firmware plus the shared tracker core. GPS fixes become !MT1 on one private channel.
void trackerBridgeBegin(MyMesh& mesh);
void trackerBridgeLoop(MyMesh& mesh);
void trackerBridgeCopyStatus(MyMesh& mesh, TrackerScreenInfo* out);
// Called from the companion radio log hooks. Repeaters retransmit the ciphertext unchanged.
void trackerBridgeOnRx(mesh::Packet* pkt);
void trackerBridgeOnTx(mesh::Packet* pkt);
