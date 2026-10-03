#pragma once

#include <stdint.h>

class MyMesh;

struct TrackerScreenInfo {
  char role[5];
  char channel[16];
  char uid[9];
  uint8_t slot;
  bool keyed;
  bool sent;
  double lat;
  double lon;
  uint32_t sequence;
  uint32_t age_s;
};

// Companion firmware plus the shared tracker core. GPS fixes become !MT1 on one private channel.
void trackerBridgeBegin(MyMesh& mesh);
void trackerBridgeLoop(MyMesh& mesh);
void trackerBridgeCopyStatus(MyMesh& mesh, TrackerScreenInfo* out);
