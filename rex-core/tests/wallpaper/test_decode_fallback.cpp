// DecodePathUnusable predicate tests (P2-1 latch, re-landed stepwise).
//
// Zero frames decoded after `budget` DecodeFrame attempts (or a send-fail
// streak on a never-decoded path) means the path is unusable, not slow:
// surface it so the supervisor can poison-park the file instead of
// stalling on it forever. Attempts reset on the first decoded frame, so a
// slow-but-working path never trips this.
//
// NOTE: the per-codec GUID probe + its tests re-land separately (Step 2)
// with string-parsed constants; this file covers only the latch predicate.

#include <gtest/gtest.h>

#include "wallpaper/decoder/FfmpegVideoDecoder.hpp"

using rex::wallpaper::DecodePathUnusable;

TEST(DecodePathUnusable, ZeroFramesPastBudgetIsUnusable) {
  EXPECT_TRUE(DecodePathUnusable(0, 240, 240));
  EXPECT_TRUE(DecodePathUnusable(0, 1000, 240));
}

TEST(DecodePathUnusable, SlowButWorkingNeverTrips) {
  EXPECT_FALSE(DecodePathUnusable(0, 239, 240));
  EXPECT_FALSE(DecodePathUnusable(1, 10000, 240));
  EXPECT_FALSE(DecodePathUnusable(5, 0, 240));
}

TEST(DecodePathUnusable, CustomBudget) {
  EXPECT_TRUE(DecodePathUnusable(0, 8, 8));
  EXPECT_FALSE(DecodePathUnusable(0, 7, 8));
}
