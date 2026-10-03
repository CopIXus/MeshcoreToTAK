#pragma once

class MyMesh;

// Companion firmware plus the shared tracker core. GPS fixes become !MT1 on one private channel.
void trackerBridgeBegin(MyMesh& mesh);
void trackerBridgeLoop(MyMesh& mesh);
