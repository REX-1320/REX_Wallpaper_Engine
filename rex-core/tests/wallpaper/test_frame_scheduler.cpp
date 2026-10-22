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

TEST(FrameSchedulerTest, FS12_PendingCountTracksUnconsumedClamped) {    // Queue-depth park (Phase 2) relies on this: unconsumed count now,
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

TEST(FrameSchedulerTest, FS13_StarvedDropsCountedExactlyOnce) {
    // 20 pushed, none consumed: 19 drop, 1 presents. A second identical
    // Tick adds nothing (regression: starved Ticks re-added the same
    // stale gap until the counter exploded 25x).
    FrameScheduler scheduler;
    for (int i = 0; i < 20; ++i) {
        VideoFrame f{};
        f.valid = true;
        f.slot = i % 8;
        f.frameIndex = i + 1;
        f.pts = i * 0.016;
        scheduler.Push(f);
    }
    const VideoFrame* result = scheduler.Tick(10.0);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->frameIndex, 20u);
    EXPECT_EQ(scheduler.GetDroppedCount(), 19ULL);
    EXPECT_EQ(scheduler.Tick(10.0), nullptr);
    EXPECT_EQ(scheduler.GetDroppedCount(), 19ULL);
}

TEST(FrameSchedulerTest, FS14_FutureFramesNotDroppedThenDue) {
    // All-future Tick counts nothing; a later Tick selects and counts
    // only the bypassed.
    FrameScheduler scheduler;
    for (int i = 0; i < 3; ++i) {
        VideoFrame f{};
        f.valid = true;
        f.slot = i;
        f.frameIndex = i + 1;
        f.pts = 1.0 + i * 0.016;
        scheduler.Push(f);
    }
    EXPECT_EQ(scheduler.Tick(0.0), nullptr);
    EXPECT_EQ(scheduler.GetDroppedCount(), 0ULL);
    const VideoFrame* result = scheduler.Tick(1.05);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->frameIndex, 3u);
    EXPECT_EQ(scheduler.GetDroppedCount(), 2ULL);
}

TEST(FrameSchedulerTest, FS15_FullRingOfFutureIsRetained) {
    // Decode lapped a loop behind the wrapped wall: 8 unpublished frames,
    // all still future. The old contract fell back to "newest published"
    // here (hence the old name) and presented frame 8 — a repeat Tick adds
    // nothing. The fixed contract RETAINS the frames instead.
    FrameScheduler scheduler;
    for (int i = 0; i < 8; ++i) {
        VideoFrame f{};
        f.valid = true;
        f.slot = i;
        f.frameIndex = i + 1;
        f.pts = 100.0 + i * 0.016;
        scheduler.Push(f);
    }
    // All eight frames are in the future of playhead 5.0 (earliest is 100.0).
    //
    // The old contract fell back to "newest published" here and presented
    // frame 8. That is exactly the defect this milestone fixes: a full ring of
    // future frames is the NORMAL state with a decode-ahead reserve, and
    // displaying one makes the content run ahead of the playhead (the ~2x
    // speed symptom). The frame is retained instead.
    EXPECT_EQ(scheduler.Tick(5.0), nullptr)
        << "a full ring of FUTURE frames must not be presented";
    EXPECT_EQ(scheduler.GetDroppedCount(), 0ULL)
        << "retaining the current frame is not a drop";
    EXPECT_EQ(scheduler.PendingCount(), 8u)
        << "future frames must be retained for later presentation";
    EXPECT_EQ(scheduler.Tick(5.0), nullptr);
    EXPECT_EQ(scheduler.GetDroppedCount(), 0ULL);
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

// ── Audit 2026-10-17, §5.4 #5: replay a non-looping video after EOF ─────

TEST(PlayAfterEofTest, EosLatchForcesRewindEvenAFrameShort) {
    // Natural EOF: the last published PTS stops one frame short of the
    // duration, so the position heuristic alone reads "mid-stream" and
    // Play() used to refuse to restart the finished video. The explicit
    // EOS latch is what makes the restart decision correct.
    const double duration = 21.9;
    const double lastPts = duration - 1.0 / 24.0;  // one 24 fps frame short
    EXPECT_FALSE(
        VideoPlaybackController::NeedsRewindOnPlay(false, lastPts, duration));
    EXPECT_TRUE(
        VideoPlaybackController::NeedsRewindOnPlay(true, lastPts, duration));
}

TEST(PlayAfterEofTest, PositionHeuristicStillRewindsAtStreamEnd) {
    // Untagged end state (a decoder that stopped without latching EOS):
    // the tail-position heuristic still covers a position at the end.
    EXPECT_TRUE(VideoPlaybackController::NeedsRewindOnPlay(false, 21.9, 21.9));
}

TEST(PlayAfterEofTest, MidStreamPlayDoesNotRewind) {
    EXPECT_FALSE(VideoPlaybackController::NeedsRewindOnPlay(false, 5.0, 100.0));
    EXPECT_FALSE(VideoPlaybackController::NeedsRewindOnPlay(false, 0.0, 100.0));
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

// ── Scene end: the playlist's "when the current ends" trigger ─────────────

TEST(SceneEndTest, LastFrameReachesTheEnd) {
    // The last published PTS of a stream sits ONE FRAME SHORT of the
    // duration (the position cannot express "finished"), so the end is
    // the frame whose PTS plus its own duration reaches the duration.
    const double duration = 56.0;
    const double fps = 30.0;
    EXPECT_TRUE(VideoPlaybackController::ReachedEndOfContent(
        duration - 1.0 / fps, duration, fps));
}

TEST(SceneEndTest, MidStreamFrameIsNotTheEnd) {
    EXPECT_FALSE(VideoPlaybackController::ReachedEndOfContent(
        10.0, 56.0, 30.0));
    EXPECT_FALSE(VideoPlaybackController::ReachedEndOfContent(
        0.0, 56.0, 30.0));
}

TEST(SceneEndTest, BackwardJumpIsNeverTheEnd) {
    // A seek (the app's playlist change re-seeks the controller) must
    // never look like "the video ended" - that would run the change loop
    // away (change -> seek -> ENDED -> change -> seek -> ...).
    EXPECT_FALSE(VideoPlaybackController::ReachedEndOfContent(
        0.0, 56.0, 30.0));
    EXPECT_FALSE(VideoPlaybackController::ReachedEndOfContent(
        55.0, 56.0, 30.0));
}

TEST(SceneEndTest, UnknownDurationNeverEnds) {
    // A stream without a duration (live/streamed) has no end.
    EXPECT_FALSE(VideoPlaybackController::ReachedEndOfContent(
        100.0, 0.0, 30.0));
}

TEST(SceneEndTest, NegativePtsIsNotTheEnd) {
    EXPECT_FALSE(VideoPlaybackController::ReachedEndOfContent(
        -1.0, 56.0, 30.0));
}

// The latch is the app-visible signal (one ENDED per scene end), so the
// state machine itself is what these tests pin - not just the comparison.
TEST(SceneEndLatchTest, SetsOnceAndStaysSetUntilRearmed) {
    SceneEndLatch latch;
    EXPECT_FALSE(latch.Ended());
    // The frame ONE FRAME SHORT of the duration is the end.
    EXPECT_TRUE(latch.OnFrame(56.0 - 1.0 / 30.0, 0.0, 56.0, 30.0));
    EXPECT_TRUE(latch.Ended());
    // Later frames of the same scene do not report a second end.
    EXPECT_FALSE(latch.OnFrame(56.0, 0.0, 56.0, 30.0));
    EXPECT_FALSE(latch.OnFrame(0.0, 0.0, 56.0, 30.0));  // loop wrap frame
    EXPECT_TRUE(latch.Ended());
}

TEST(SceneEndLatchTest, MidStreamFrameDoesNotSetIt) {
    SceneEndLatch latch;
    EXPECT_FALSE(latch.OnFrame(0.0, 0.0, 56.0, 30.0));
    EXPECT_FALSE(latch.OnFrame(55.0, 0.0, 56.0, 30.0));
    EXPECT_FALSE(latch.Ended());
}

TEST(SceneEndLatchTest, SeekRearmsTheSignal) {
    // The app's playlist change re-seeks the controller. A latch that
    // survived the seek would report the same end again and run the
    // change loop away (change -> seek -> ENDED -> change -> ...).
    SceneEndLatch latch;
    ASSERT_TRUE(latch.OnFrame(56.0 - 1.0 / 30.0, 0.0, 56.0, 30.0));
    latch.Rearm();  // Seek(0.0)
    EXPECT_FALSE(latch.Ended());
    // Re-armed means the NEXT end is a new scene end.
    EXPECT_TRUE(latch.OnFrame(56.0 - 1.0 / 30.0, 0.0, 56.0, 30.0));
    EXPECT_TRUE(latch.Ended());
}

TEST(SceneEndLatchTest, ContentStartIsSubtracted) {
    // Frame PTS are absolute stream timestamps: a stream whose start_time
    // is non-zero must not look "ended" on its first frame, and its last
    // frame must still reach the end.
    SceneEndLatch latch;
    EXPECT_FALSE(latch.OnFrame(1200.0, 1200.0, 56.0, 30.0));
    EXPECT_FALSE(latch.Ended());
    EXPECT_TRUE(latch.OnFrame(1200.0 + 56.0 - 1.0 / 30.0, 1200.0, 56.0, 30.0));
    EXPECT_TRUE(latch.Ended());
}

TEST(FrameSchedulerPrimeTest, FreshSchedulerIsUnprimed) {
    // Nothing consumed yet: the burst gate must hold (decode paces 1x).
    FrameScheduler s;
    EXPECT_FALSE(s.IsPrimed());
}

TEST(FrameSchedulerPrimeTest, ConsumePrimesStarveDoesNot) {
    FrameScheduler s;
    VideoFrame future{};
    future.valid = true;
    future.slot = 0;
    future.frameIndex = 1;
    future.pts = 100.0;  // far future: Tick starves, must not prime
    s.Push(future);
    EXPECT_EQ(s.Tick(0.0), nullptr);
    EXPECT_FALSE(s.IsPrimed());
    VideoFrame due{};
    due.valid = true;
    due.slot = 1;
    due.frameIndex = 2;
    due.pts = 0.0;
    s.Push(due);
    ASSERT_NE(s.Tick(0.0), nullptr);
    EXPECT_TRUE(s.IsPrimed());
}

TEST(FrameSchedulerPrimeTest, ClearDisarmsUntilNextConsume) {
    FrameScheduler s;
    VideoFrame a{};
    a.valid = true;
    a.slot = 0;
    a.frameIndex = 1;
    s.Push(a);
    ASSERT_NE(s.Tick(0.0), nullptr);
    EXPECT_TRUE(s.IsPrimed());
    s.Clear();
    EXPECT_FALSE(s.IsPrimed());
    // Post-clear pushes do not re-prime by themselves.
    VideoFrame b{};
    b.valid = true;
    b.slot = 1;
    b.frameIndex = 2;
    s.Push(b);
    EXPECT_FALSE(s.IsPrimed());
    ASSERT_NE(s.Tick(0.0), nullptr);
    EXPECT_TRUE(s.IsPrimed());
}

TEST(FrameSchedulerPrimeTest, DisarmHoldsBufferButDropsSprint) {
    // Resume path: DisarmPrime keeps consumable frames (unlike Clear) and
    // only withholds the burst until the next consume.
    FrameScheduler s;
    VideoFrame a{};
    a.valid = true;
    a.slot = 0;
    a.frameIndex = 1;
    s.Push(a);
    ASSERT_NE(s.Tick(0.0), nullptr);
    EXPECT_TRUE(s.IsPrimed());
    s.DisarmPrime();
    EXPECT_FALSE(s.IsPrimed());
    EXPECT_EQ(s.PendingCount(), 0u);  // consumed, nothing pending
    VideoFrame b{};
    b.valid = true;
    b.slot = 1;
    b.frameIndex = 2;
    s.Push(b);
    EXPECT_EQ(s.PendingCount(), 1u);  // buffered, still consumable
    ASSERT_NE(s.Tick(0.0), nullptr);
    EXPECT_TRUE(s.IsPrimed());
}

// ── Audit 2026-10-17, §5.4 #4: Clear vs a concurrently ticking consumer ──

TEST(FrameSchedulerClearRaceTest, ClearSuppressesUntilNextPush) {
    // Deterministic contract: after Clear and with no further pushes the
    // ring is empty (PendingCount 0, Tick nullptr) and the burst gate is
    // disarmed — no matter how many Ticks pass. Post-clear pushes are
    // consumable and re-arm the gate.
    FrameScheduler s;
    for (int i = 0; i < 4; ++i) {
        VideoFrame f{};
        f.valid = true;
        f.slot = static_cast<uint32_t>(i % 8);
        f.frameIndex = static_cast<uint64_t>(i + 1);
        f.pts = i * 0.02;
        s.Push(f);
    }
    ASSERT_NE(s.Tick(0.1), nullptr);
    s.Clear();
    EXPECT_EQ(s.PendingCount(), 0u);
    EXPECT_FALSE(s.IsPrimed());
    EXPECT_EQ(s.Tick(1e9), nullptr)
        << "no pre-clear frame may reappear after Clear";
    EXPECT_EQ(s.Tick(1e9), nullptr);

    VideoFrame nf{};
    nf.valid = true;
    nf.slot = 0;
    nf.frameIndex = 99;
    nf.pts = 0.0;
    s.Push(nf);
    EXPECT_EQ(s.PendingCount(), 1u);
    ASSERT_NE(s.Tick(0.0), nullptr);
    EXPECT_TRUE(s.IsPrimed());  // consume in the CURRENT epoch re-arms
}

TEST(FrameSchedulerClearRaceTest, RacingTickCannotRollBackClear) {
    // The reported defect: a Tick whose scan straddled a Clear() stored its
    // stale watermark AFTER the Clear's re-baseline — "un-clearing" the
    // ring (pre-clear frames consumable again) and re-arming the burst
    // gate. The commit is now forward-only and the gate epoch-paired, so
    // once the last Clear and the racing consumer have both settled, the
    // ring must be empty and the gate disarmed.
    for (int round = 0; round < 40; ++round) {
        FrameScheduler s;
        std::atomic<bool> stop{false};
        std::thread ticker([&]() {
            double t = 0.0;
            while (!stop.load(std::memory_order_acquire)) {
                s.Tick(t);
                // Sweep the full PTS range the round publishes (up to
                // ~5.1 s) so the ticker is mid-scan — not starved — for
                // every Clear, including the final one whose rollback
                // would otherwise be masked by a later re-baseline.
                t += 0.05;
                if (t > 5.5) {
                    t = 0.0;
                }
            }
        });
        for (int i = 0; i < 64; ++i) {
            for (int j = 0; j < 4; ++j) {
                VideoFrame f{};
                f.valid = true;
                f.slot = static_cast<uint32_t>((i * 4 + j) % 8);
                f.frameIndex = static_cast<uint64_t>(i * 4 + j + 1);
                f.pts = i * 0.08 + j * 0.02;
                s.Push(f);
            }
            s.Clear();  // seek/loop-wrap shaped: re-baseline every burst
        }
        stop.store(true, std::memory_order_release);
        ticker.join();

        EXPECT_EQ(s.PendingCount(), 0u)
            << "round " << round
            << ": clear re-baseline must never be rolled back";
        EXPECT_FALSE(s.IsPrimed())
            << "round " << round << ": burst gate must stay disarmed after Clear";
        EXPECT_EQ(s.Tick(1e9), nullptr)
            << "round " << round << ": no pre-clear frame may reappear after Clear";
    }
}

TEST(FrameSchedulerClearRaceTest, ClearInsideTickScanIsSuppressed) {
    // Deterministic interleave for the §5.4 #4 defect: the hook fires on
    // the consumer thread after the scan has selected a frame and before
    // the commit — exactly the window in which a Clear() from the
    // command/decode thread used to be rolled back by the unconditional
    // store. The selection made from pre-clear state must be suppressed,
    // and Clear's re-baseline must stand.
    FrameScheduler s;
    for (int i = 0; i < 3; ++i) {
        VideoFrame f{};
        f.valid = true;
        f.slot = static_cast<uint32_t>(i);
        f.frameIndex = static_cast<uint64_t>(i + 1);
        f.pts = i * 0.02;  // due at target 0.05
        s.Push(f);
    }
    VideoFrame futureFrame{};
    futureFrame.valid = true;
    futureFrame.slot = 3;
    futureFrame.frameIndex = 4;
    // Never due at target 0.05: the scan must select frame 3, not the
    // newest write, so the commit straddles the Clear below.
    futureFrame.pts = 100.0;
    s.Push(futureFrame);

    FrameScheduler* target = &s;
    s.SetTestBeforeCommitHook(
        [](void* ctx) { static_cast<FrameScheduler*>(ctx)->Clear(); }, target);

    const VideoFrame* f = s.Tick(0.05);
    EXPECT_EQ(f, nullptr)
        << "a commit that straddled a Clear must be suppressed";
    EXPECT_EQ(s.PendingCount(), 0u) << "Clear's re-baseline must stand";
    EXPECT_FALSE(s.IsPrimed()) << "the burst gate must stay disarmed";

    s.SetTestBeforeCommitHook(nullptr, nullptr);
    // The scheduler keeps working normally after the injected race.
    VideoFrame next{};
    next.valid = true;
    next.slot = 0;
    next.frameIndex = 5;
    next.pts = 0.03;
    s.Push(next);
    ASSERT_NE(s.Tick(0.05), nullptr);
    EXPECT_TRUE(s.IsPrimed());
}

} // namespace
} // namespace rex::wallpaper
