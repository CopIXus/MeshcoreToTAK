#include <gtest/gtest.h>

#include "TakSeen.h"

TEST(TakSeen, HashAndMeshUid) {
  EXPECT_TRUE(TakSeen::isMeshUid("meshtracker-A1B2C3D4"));
  EXPECT_TRUE(TakSeen::isMeshUid("meshcore-0123456789abcdef"));
  EXPECT_FALSE(TakSeen::isMeshUid("ANDROID-123"));
  EXPECT_FALSE(TakSeen::isMeshUid(""));
  EXPECT_NE(TakSeen::hashUid("meshtracker-A1B2C3D4"), 0u);
  EXPECT_EQ(TakSeen::hashUid("meshtracker-A1B2C3D4"), TakSeen::hashUid("meshtracker-A1B2C3D4"));
}

TEST(TakSeen, ParseTimeMs) {
  int64_t ms = TakSeen::parseTimeMs("2026-11-16T14:32:05.000Z");
  ASSERT_GT(ms, 0);
  EXPECT_EQ(ms % 1000, 0);
  EXPECT_EQ(TakSeen::parseTimeMs("not-a-time"), 0);
}

TEST(TakSeen, Ordering) {
  TakSeen s;
  const char* uid = "meshtracker-AABBCCDD";
  EXPECT_TRUE(s.isNewer(uid, 1000));
  s.note(uid, 1000);
  EXPECT_FALSE(s.isNewer(uid, 1000));
  EXPECT_FALSE(s.isNewer(uid, 999));
  EXPECT_TRUE(s.isNewer(uid, 1001));
  s.note(uid, 1001);
  EXPECT_TRUE(s.sawPeer());
  EXPECT_FALSE(s.isNewer(uid, 1001));
}

TEST(TakSeen, IgnoresNonMesh) {
  TakSeen s;
  s.note("ANDROID-1", 5000);
  EXPECT_FALSE(s.sawPeer());
  EXPECT_TRUE(s.isNewer("ANDROID-1", 1));
}
