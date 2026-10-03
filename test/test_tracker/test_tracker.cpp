#include <gtest/gtest.h>

#include "TakTracker.h"

TEST(TakTrackerParse, AcceptsCanonicalFix) {
  TakTrackerMessage m;
  const char* text = "!MT1;u=A1B2C3D4;k=k9;la=36.123456;ln=-82.123456;s=1.2;c=90;a=512;st=30;q=1042;b=87";
  ASSERT_TRUE(TakTracker::parse(text, m));
  EXPECT_STREQ("A1B2C3D4", m.id);
  EXPECT_STREQ("k9", m.role);
  EXPECT_NEAR(36.123456, m.lat, 1e-6);
  EXPECT_NEAR(-82.123456, m.lon, 1e-6);
  EXPECT_TRUE(m.has_speed);
  EXPECT_NEAR(1.2f, m.speed_mps, 0.01f);
  EXPECT_TRUE(m.has_course);
  EXPECT_NEAR(90.0f, m.course_deg, 0.01f);
  EXPECT_TRUE(m.has_altitude);
  EXPECT_NEAR(512.0f, m.altitude_m, 0.01f);
  EXPECT_EQ(30, m.stale_sec);
  EXPECT_EQ(1042u, m.sequence);
  EXPECT_TRUE(m.has_battery);
  EXPECT_EQ(87, m.battery_pct);
}

TEST(TakTrackerParse, RequiresRoleAndCoreFields) {
  TakTrackerMessage m;
  EXPECT_TRUE(TakTracker::parse("!MT1;u=A1B2C3D4;k=veh;la=36.1;ln=-82.1;st=10;q=1", m));
  EXPECT_STREQ("veh", m.role);
  EXPECT_FALSE(m.has_speed);
  EXPECT_FALSE(m.has_battery);
  EXPECT_FALSE(TakTracker::parse("!MT1;u=A1B2C3D4;la=36.1;ln=-82.1;st=10;q=1", m));
  EXPECT_FALSE(TakTracker::parse("!MT1;k=k9;la=36.1;ln=-82.1;st=10;q=1", m));
  EXPECT_FALSE(TakTracker::parse("!MT1;u=A1B2C3D4;k=k9;ln=-82.1;st=10;q=1", m));
  EXPECT_FALSE(TakTracker::parse("!MT1;u=A1B2C3D4;k=k9;la=36.1;st=10;q=1", m));
  EXPECT_FALSE(TakTracker::parse("!MT1;u=A1B2C3D4;k=k9;la=36.1;ln=-82.1;q=1", m));
  EXPECT_FALSE(TakTracker::parse("!MT1;u=A1B2C3D4;k=k9;la=36.1;ln=-82.1;st=10", m));
}

TEST(TakTrackerParse, RejectsBadNumbersAndVersions) {
  TakTrackerMessage m;
  EXPECT_FALSE(TakTracker::isTrackerMessage("hello"));
  EXPECT_FALSE(TakTracker::parse("!MT2;u=A1B2C3D4;k=k9;la=36.1;ln=-82.1;st=10;q=1", m));
  EXPECT_TRUE(TakTracker::isTrackerMessage("!MT1;nope"));
  EXPECT_FALSE(TakTracker::parse("!MT1;nope", m));
  EXPECT_FALSE(TakTracker::parse("!MT1;u=A1B2C3D4;k=k9;la=999;ln=-82.1;st=10;q=1", m));
  EXPECT_FALSE(TakTracker::parse("!MT1;u=A1B2C3D4;k=k9;la=36.1;ln=-999;st=10;q=1", m));
  EXPECT_FALSE(TakTracker::parse("!MT1;u=A1B2C3D4;k=k9;la=0;ln=0;st=10;q=1", m));
  EXPECT_FALSE(TakTracker::parse("!MT1;u=A1B2C3D4;k=k9;la=36.1;ln=-82.1;st=0;q=1", m));
  EXPECT_FALSE(TakTracker::parse("!MT1;u=A1B2C3D4;k=k9;la=36.1;ln=-82.1;st=4;q=1", m));
  EXPECT_FALSE(TakTracker::parse("!MT1;u=A1B2C3D4;k=k9;la=36.1;ln=-82.1;st=3601;q=1", m));
  EXPECT_FALSE(TakTracker::parse("!MT1;u=A1B2C3D4;k=k9;la=36.1;ln=-82.1;st=10;q=abc", m));
  EXPECT_FALSE(TakTracker::parse("!MT1;u=A1B2C3D4;k=k9;la=36.1;ln=-82.1;st=10;q=-1", m));
  EXPECT_FALSE(TakTracker::parse("!MT1;u=ZZ;k=k9;la=36.1;ln=-82.1;st=10;q=1", m));
  EXPECT_FALSE(TakTracker::parse("!MT1;u=A1B2C3D4;k=;la=36.1;ln=-82.1;st=10;q=1", m));
  EXPECT_FALSE(TakTracker::parse("!MT1;u=A1B2C3D4;k=k;la=36.1;ln=-82.1;st=10;q=1", m));
  EXPECT_FALSE(TakTracker::parse("!MT1;u=A1B2C3D4;k=k9;la=36.1;ln=-82.1;s=-1;st=10;q=1", m));
  EXPECT_FALSE(TakTracker::parse("!MT1;u=A1B2C3D4;k=k9;la=36.1;ln=-82.1;c=360;st=10;q=1", m));
  EXPECT_FALSE(TakTracker::parse("!MT1;u=A1B2C3D4;k=k9;la=36.1;ln=-82.1;b=101;st=10;q=1", m));
}

TEST(TakTrackerParse, IgnoresUnknownAndRejectsDuplicates) {
  TakTrackerMessage m;
  ASSERT_TRUE(TakTracker::parse("!MT1;u=a1b2c3d4;k=PER;la=36.1;ln=-82.1;st=10;q=1;x=future", m));
  EXPECT_STREQ("A1B2C3D4", m.id);
  EXPECT_STREQ("per", m.role);
  EXPECT_FALSE(TakTracker::parse("!MT1;u=A1B2C3D4;u=A1B2C3D4;k=k9;la=36.1;ln=-82.1;st=10;q=1", m));
  EXPECT_FALSE(TakTracker::parse("!MT1;u=A1B2C3D4;k=k9;la=36.1;ln=-82.1;st=10;q=1;q=2", m));
}

TEST(TakTrackerSequence, DuplicateNewerOlderAndWrap) {
  EXPECT_EQ(TakSeqResult::Accept, TakTracker::sequence(false, 0, 1));
  EXPECT_EQ(TakSeqResult::Duplicate, TakTracker::sequence(true, 10, 10));
  EXPECT_EQ(TakSeqResult::Accept, TakTracker::sequence(true, 10, 11));
  EXPECT_EQ(TakSeqResult::Older, TakTracker::sequence(true, 10, 9));
  EXPECT_EQ(TakSeqResult::Accept, TakTracker::sequence(true, 0xFFFFFFF0u, 0x10u));
  EXPECT_EQ(TakSeqResult::Older, TakTracker::sequence(true, 0x10u, 0xFFFFFFF0u));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
