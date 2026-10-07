#include <gtest/gtest.h>
#include "wallpaper/playback/FrameScheduler.hpp"
#include "wallpaper/playback/VideoPlaybackController.hpp"
#include <algorithm>
#include <thread>
#include <atomic>

namespace rex::wallpaper {
namespace {

TEST(FrameSchedulerTest, FS01_FirstFrameIsNotSkipped) {
    FrameScheduler scheduler;
    VideoFrame frame{};
    frame.valid = true;
    frame.slot = 0;
    frame.frameIndex = 1;
    scheduler.Push(frame);

    const VideoFrame* result = scheduler.Tick(0.0);
    ASSERT_NE(result, nullptr);
    EXPECT_TRUE(result->valid);
    EXPECT_EQ(result->frameIndex, 1u);
}

TEST(FrameSchedulerTest, FS02_LatestWinsSemantics) {
    FrameScheduler scheduler;

    VideoFrame frameA{};
    frameA.valid = true;
    frameA.slot = 0;
    frameA.frameIndex = 1;
    scheduler.Push(frameA);

    VideoFrame frameB{};
    frameB.valid = true;
    frameB.slot = 1;
    frameB.frameIndex = 2;
    scheduler.Push(frameB);

    const VideoFrame* result = scheduler.Tick(0.0);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->frameIndex, 2u);
}

TEST(FrameSchedulerTest, FS03_NoNewFrameReturnsNullptr) {
    FrameScheduler scheduler;
    VideoFrame frame{};
    frame.valid = true;
    frame.slot = 0;
    frame.frameIndex = 1;
    scheduler.Push(frame);

    const VideoFrame* first = scheduler.Tick(0.0);
    ASSERT_NE(first, nullptr);

    const VideoFrame* second = scheduler.Tick(0.0);
    EXPECT_EQ(second, nullptr);
}

TEST(FrameSchedulerTest, FS04_RingWrapAround) {
    FrameScheduler scheduler;
    // Push more frames than the ring size (kNumSlots = 8)
    for (int i = 0; i < 10; ++i) {
        VideoFrame frame{};
        frame.valid = true;
        frame.slot = i % 8;
        frame.frameIndex = i + 1;
        scheduler.Push(frame);
    }

    const VideoFrame* result = scheduler.Tick(0.0);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->frameIndex, 10u);
}

TEST(FrameSchedulerTest, FS05_ThreadSafety) {
    FrameScheduler scheduler;
    std::atomic<int> pushCount{0};
    std::atomic<int> tickCount{0};

    std::thread producer([&]() {
        for (int i = 0; i < 1000; ++i) {
            VideoFrame frame{};
            frame.valid = true;
            frame.slot = i % 8;
            frame.frameIndex = i + 1;
            scheduler.Push(frame);
            pushCount.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::thread consumer([&]() {
        for (int i = 0; i < 1000; ++i) {
            scheduler.Tick(0.0);
            tickCount.fetch_add(1, std::memory_order_relaxed);
        }
    });

    producer.join();
    consumer.join();

    // No crash means success; verify both threads ran
    EXPECT_EQ(pushCount.load(), 1000);
    EXPECT_EQ(tickCount.load(), 1000);
}

TEST(FrameSchedulerTest, FS06_ClearDropsStaleFrame) {
    // P1-5 / R10: after a seek the pre-seek frame must not be presented.
    // Clear() makes Tick return nullptr until the next Push.
    FrameScheduler scheduler;

    VideoFrame frameA{};
    frameA.valid = true;
    frameA.slot = 0;
    frameA.frameIndex = 1;
    scheduler.Push(frameA);
    ASSERT_NE(scheduler.Tick(0.0), nullptr);

    scheduler.Clear();
    EXPECT_EQ(scheduler.Tick(0.0), nullptr);

    VideoFrame frameB{};
    frameB.valid = true;
    frameB.slot = 1;
    frameB.frameIndex = 2;
    scheduler.Push(frameB);
    const VideoFrame* result = scheduler.Tick(0.0);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->frameIndex, 2u);
}

TEST(FrameSchedulerTest, FS07_ClearOnEmptyIsHarmless) {

    FrameScheduler scheduler;
    scheduler.Clear();
    EXPECT_EQ(scheduler.Tick(0.0), nullptr);
}

// ═══════════════════════════════════════════════════════════════════════════
// PTS-aware selection (Phase 2: clock separation)
// ═══════════════════════════════════════════════════════════════════════════
//
// Tick answers "which decoded frame is due at wall time T": the newest
// frame with pts <= target (+8 ms tolerance). Bypassed and overwritten-
// unseen frames count as dropped; equal-PTS repeats are free.

TEST(FrameSchedulerTest, FS08_SelectsNewestDueFrame) {
    FrameScheduler scheduler;
    for (int i = 0; i < 3; ++i) {
        VideoFrame f{};
        f.valid = true;
        f.slot = i;
        f.frameIndex = i + 1;
        f.pts = i * 0.016;
        scheduler.Push(f);
    }
    const VideoFrame* result = scheduler.Tick(0.02);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->frameIndex, 2u);  // pts 0.016 due, pts 0.032 future
}

TEST(FrameSchedulerTest, FS09_FutureFrameNotDue) {
    FrameScheduler scheduler;
    VideoFrame f{};
    f.valid = true;
    f.slot = 0;
    f.frameIndex = 1;
    f.pts = 1.0;
    scheduler.Push(f);
    EXPECT_EQ(scheduler.Tick(0.0), nullptr);
    const VideoFrame* result = scheduler.Tick(1.0);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->frameIndex, 1u);
}

TEST(FrameSchedulerTest, FS10_SkippedFramesCountAsDropped) {
    FrameScheduler scheduler;
    for (int i = 0; i < 4; ++i) {
        VideoFrame f{};
        f.valid = true;
        f.slot = i;
        f.frameIndex = i + 1;
        f.pts = i * 0.016;
        scheduler.Push(f);
    }
    const VideoFrame* result = scheduler.Tick(0.05);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->frameIndex, 4u);
    EXPECT_EQ(scheduler.GetDroppedCount(), 3ULL);
}

TEST(FrameSchedulerTest, FS11_EqualPtsRepeatIsNotADrop) {    FrameScheduler scheduler;
    for (int i = 0; i < 2; ++i) {
        VideoFrame f{};
        f.valid = true;
        f.slot = i;
        f.frameIndex = i + 1;
        f.pts = 1.0;
        scheduler.Push(f);
    }
    const VideoFrame* result = scheduler.Tick(1.0);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->frameIndex, 2u);
    EXPECT_EQ(scheduler.GetDroppedCount(), 0ULL);
}

TEST(FrameSchedulerTest, FS12_PendingCountTracksUnconsumedClamped) {
    // Queue-depth park (Phase 2) relies on this: unconsumed count now,
    // clamped to the ring size once overwrites begin.
    FrameScheduler scheduler;
    EXPECT_EQ(scheduler.PendingCount(), 0U);
    for (int i = 0; i < 3; ++i) {
        VideoFrame f{};
        f.valid = true;
        f.slot = i;
        f.frameIndex = i + 1;
        f.pts = i * 0.016;
        scheduler.Push(f);
    }
    EXPECT_EQ(scheduler.PendingCount(), 3U);
    EXPECT_NE(scheduler.Tick(10.0), nullptr);
    EXPECT_EQ(scheduler.PendingCount(), 0U);
    for (int i = 0; i < 20; ++i) {
        VideoFrame f{};
        f.valid = true;
        f.slot = i % 8;
        f.frameIndex = i + 1;
        f.pts = i * 0.016;
        scheduler.Push(f);
    }
    EXPECT_EQ(scheduler.PendingCount(), 8U);
}


TEST(DiscontinuityRuleTest, BackwardJumpIsASeek) {
    // Loop wrap (duration -> ~0) must clear the scheduler.
    EXPECT_TRUE(VideoPlaybackController::IsBackwardJump(12.0, 0.04));
    EXPECT_TRUE(VideoPlaybackController::IsBackwardJump(1.48, 0.0));
}

TEST(DiscontinuityRuleTest, ForwardProgressIsNotASeek) {
    EXPECT_FALSE(VideoPlaybackController::IsBackwardJump(1.0, 1.04));
    EXPECT_FALSE(VideoPlaybackController::IsBackwardJump(-1.0, 0.0));
}

TEST(DiscontinuityRuleTest, EqualPtsRepeatIsNotASeek) {
    // Frame repeats (same PTS twice) must NOT drop the frame.
    EXPECT_FALSE(VideoPlaybackController::IsBackwardJump(1.0, 1.0));
}

TEST(DiscontinuityRuleTest, BFrameReorderJitterIsNotASeek) {
    // Decode order for B-frame content steps PTS backward by a frame or
    // two constantly (0.033 -> 0.016 -> ...): clearing on those would
    // churn the scheduler every other frame.
    EXPECT_FALSE(VideoPlaybackController::IsBackwardJump(0.033, 0.016));
    EXPECT_FALSE(VideoPlaybackController::IsBackwardJump(1.0, 0.983));
}

TEST(DiscontinuityRuleTest, ShortLoopWrapIsASeek) {
    // A sub-second looping file wraps 0.45 -> ~0: small jump, but it
    // lands at stream start, so it still clears.
    EXPECT_TRUE(VideoPlaybackController::IsBackwardJump(0.45, 0.0));
}

TEST(WrapTargetTest, LoopWrapsTargetByDuration) {
    // 21.9 s loop: wall 22.0 must select content at ~0.1, not starve.
    EXPECT_NEAR(VideoPlaybackController::WrapTarget(22.0, 21.9, true),
                0.1, 1e-9);
    EXPECT_NEAR(VideoPlaybackController::WrapTarget(43.9, 21.9, true),
                0.1, 1e-6);
}

TEST(WrapTargetTest, PreWrapTargetPassesThrough) {
    EXPECT_DOUBLE_EQ(VideoPlaybackController::WrapTarget(10.0, 21.9, true),
                     10.0);
}

TEST(WrapTargetTest, NonLoopingTargetPassesThrough) {
    EXPECT_DOUBLE_EQ(VideoPlaybackController::WrapTarget(50.0, 21.9, false),
                     50.0);
}

TEST(WrapTargetTest, ZeroDurationPassesThrough) {
    EXPECT_DOUBLE_EQ(VideoPlaybackController::WrapTarget(50.0, 0.0, true),
                     50.0);
}

} // namespace
} // namespace rex::wallpaper
