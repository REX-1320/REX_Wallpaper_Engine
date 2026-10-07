// ═══════════════════════════════════════════════════════════════════════════
// P1-10 regression tests: bounded EOF→seek→EOF cycle (R5)
// ═══════════════════════════════════════════════════════════════════════════
//
// DecodeFrame looped EOF → SeekInternal(0) → EOF forever on 0-frame files
// (hanging Open's synchronous prewarm), and the thread spun without sleep
// on loop+failure. The guard allows exactly one internal seek per
// DecodeFrame call — a normal loop wrap needs one; a second consecutive
// EOF means the seek produced no frames, so DecodeFrame must return.

#include <gtest/gtest.h>

#include "wallpaper/decoder/FfmpegVideoDecoder.hpp"

using namespace rex::wallpaper;

TEST(EofLoopGuardTest, FreshGuardAllowsOneSeek) {
    EofLoopGuard guard;
    EXPECT_TRUE(guard.AllowSeek());
}

TEST(EofLoopGuardTest, SecondConsecutiveSeekIsDenied) {
    EofLoopGuard guard;
    EXPECT_TRUE(guard.AllowSeek());
    EXPECT_FALSE(guard.AllowSeek());
    EXPECT_FALSE(guard.AllowSeek());
}

TEST(EofLoopGuardTest, BoundIsPerCallScope) {
    // Each DecodeFrame call gets a fresh guard: normal looping (one wrap
    // per call, frames flowing after) is never throttled.
    EofLoopGuard first;
    EXPECT_TRUE(first.AllowSeek());
    EXPECT_FALSE(first.AllowSeek());
    EofLoopGuard second;
    EXPECT_TRUE(second.AllowSeek());
}
