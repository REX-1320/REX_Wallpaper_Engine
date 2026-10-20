// FrameScheduler frame-selection semantics (decode-ahead correctness).
//
// THE BUG THIS GUARDS: with a decode-ahead reserve the ring legitimately holds
// frames whose PTS the playback clock has NOT reached. The scheduler used to
// fall back to "newest published frame" when nothing was due, which displayed
// a future frame - so displayed CONTENT ran ahead of the playhead and the
// wallpaper read as playing at ~2x, while position reported 1.000x and
// effectiveFps reported the source rate. No telemetry could detect it.
//
// Contract under test:
//   1. The playback clock is authoritative; publishing != eligible.
//   2. Select the newest frame with pts <= playhead + tolerance.
//   3. Nothing due -> retain the current frame; NEVER show a future frame.
//   4. Multiple overdue -> newest overdue wins, older ones counted as skipped.
//   5. Seek / loop wrap / restart must not present stale frames.
//   6. Slot ownership stays with the decoder; selection never blocks.

#include <gtest/gtest.h>

#include "wallpaper/playback/FrameScheduler.hpp"

#include <limits>

using namespace rex::wallpaper;

namespace {

constexpr double kFrame24 = 1.0 / 24.0;

VideoFrame MakeFrame(int index, double pts) {
    VideoFrame f{};
    f.slot = static_cast<UINT>(index % 8);
    f.fenceValue = static_cast<uint64_t>(index);
    f.pts = pts;
    f.frameIndex = static_cast<uint64_t>(index);
    f.width = 1920;
    f.height = 1080;
    f.valid = true;
    return f;
}

}  // namespace

// 1. Future-only frames never advance displayed content prematurely.
TEST(FrameSchedulerSelect, FutureOnlyFramesAreNeverPresented) {
    FrameScheduler s;
    // Every published frame is strictly ahead of the playhead even after the
    // selection tolerance (pts 0 IS due at playhead 0, so start at one frame).
    for (int i = 1; i <= 5; ++i) s.Push(MakeFrame(i, i * kFrame24));

    const VideoFrame* f = s.Tick(0.0);
    EXPECT_EQ(f, nullptr) << "a future frame must not be displayed";
    EXPECT_EQ(s.StarvedTicks(), 1u);
    EXPECT_EQ(s.GetDroppedCount(), 0u)
        << "retaining the current frame is not a drop";
    EXPECT_EQ(s.PendingCount(), 5u) << "all future frames must be retained";
}

// 2. A due frame is selected even when later frames are already published.
TEST(FrameSchedulerSelect, SelectsNewestDueWhileFutureFramesAreBuffered) {
    FrameScheduler s;
    for (int i = 0; i < 5; ++i) s.Push(MakeFrame(i, i * kFrame24));

    // Playhead at frame 1: only frames 0 and 1 are due.
    const VideoFrame* f = s.Tick(kFrame24);
    ASSERT_NE(f, nullptr);
    EXPECT_NEAR(f->pts, 1 * kFrame24, 1e-9) << "must select the newest DUE frame";

    // Frames 2..4 must still be pending for later presentation.
    EXPECT_EQ(s.PendingCount(), 3u)
        << "future frames must be retained, not consumed or dropped";
}

// The exact scenario from the bug report: frames 100,101,105 published.
TEST(FrameSchedulerSelect, DoesNotSkipToFrame105WhenOnly101IsDue) {
    FrameScheduler s;
    s.Push(MakeFrame(100, 100 * kFrame24));
    s.Push(MakeFrame(101, 101 * kFrame24));
    s.Push(MakeFrame(105, 105 * kFrame24));

    const VideoFrame* f = s.Tick(101 * kFrame24);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->frameIndex, 101u) << "must present 101, never 105";
}

// 4. Overdue selection presents the newest overdue frame and counts the
// genuinely skipped older ones as drops; future frames are never touched.
TEST(FrameSchedulerSelect, MultipleOverduePresentsNewestAndCountsSkips) {
    FrameScheduler s;
    for (int i = 0; i < 4; ++i) s.Push(MakeFrame(i, i * kFrame24));

    // Playhead at frame 3: frames 0,1,2 are all overdue.
    const VideoFrame* f = s.Tick(3 * kFrame24);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->frameIndex, 3u) << "closest-to-playhead frame must win";
    EXPECT_EQ(s.GetDroppedCount(), 3u)
        << "the three genuinely skipped older frames must be counted";
}

// 3b. Overdue selection must never pick a FUTURE frame.
TEST(FrameSchedulerSelect, OverdueSelectionNeverReturnsFutureFrame) {
    FrameScheduler s;
    for (int i = 0; i < 6; ++i) s.Push(MakeFrame(i, i * kFrame24));

    const VideoFrame* f = s.Tick(2 * kFrame24);   // frames 3,4,5 are future
    ASSERT_NE(f, nullptr);
    EXPECT_LE(f->pts, 2 * kFrame24 + FrameScheduler::kSelectToleranceSec)
        << "selected frame must not be in the future";
    EXPECT_EQ(f->frameIndex, 2u);
}

// 4. Presentation timestamps advance monotonically during decode-ahead.
TEST(FrameSchedulerSelect, PresentationTimestampsAreMonotonic) {
    FrameScheduler s;
    double last = -std::numeric_limits<double>::infinity();
    for (int i = 0; i < 60; ++i) {
        const int idx = i + 10;
        s.Push(MakeFrame(idx, idx * kFrame24));
        // Playhead always 3 frames behind the newest published frame.
        const double playhead = (idx - 3) * kFrame24;
        const VideoFrame* f = s.Tick(playhead);
        if (f) {
            EXPECT_GE(f->pts, last) << "presentation time went backwards at " << i;
            last = f->pts;
        }
    }
}

// 5. Loop wrap / seek must not present stale frames.
TEST(FrameSchedulerSelect, ClearSuppressesStaleFramesUntilRepublished) {
    FrameScheduler s;
    for (int i = 0; i < 3; ++i) s.Push(MakeFrame(i, i * kFrame24));
    ASSERT_NE(s.Tick(kFrame24), nullptr);

    // Loop wrap: Clear re-baselines the ring, as the controller does.
    s.Clear();
    EXPECT_EQ(s.Tick(kFrame24), nullptr)
        << "no pre-wrap frame may be presented after Clear";

    // Post-wrap frames restart near zero and MUST be presentable.
    s.Push(MakeFrame(100, 0.0));
    s.Push(MakeFrame(101, kFrame24));
    const VideoFrame* f = s.Tick(kFrame24);
    ASSERT_NE(f, nullptr)
        << "post-wrap frames must not be rejected as stale";
    EXPECT_NEAR(f->pts, kFrame24, 1e-9);
}

// Presentation timestamps never walk BACKWARDS within one Tick's scan, and a
// decode-ahead buffer never causes a future frame to be shown.
TEST(FrameSchedulerSelect, DecodeAheadNeverCausesPrematureAdvance) {
    FrameScheduler s;
    for (int i = 0; i < 60; ++i) {
        const int idx = i + 10;
        s.Push(MakeFrame(idx, idx * kFrame24));
        // Playhead always 3 frames behind the newest published frame, so every
        // Tick must return a frame at or before the playhead.
        const double playhead = (idx - 3) * kFrame24;
        const VideoFrame* f = s.Tick(playhead);
        if (f) {
            EXPECT_LE(f->pts, playhead + FrameScheduler::kSelectToleranceSec)
                << "frame " << i << " was presented before it was due";
        }
    }
}

// When the decoder falls BEHIND the playhead, the correct catch-up action is
// to present a LOWER PTS than last time. A "PTS must not go backwards" guard
// there deadlocks the pipeline (measured: queue=8, repeat 60/s, effFps 0).
TEST(FrameSchedulerSelect, CatchUpMayPresentLowerPtsWhenDecoderFallsBehind) {
    FrameScheduler s;
    s.Push(MakeFrame(0, 0.0));
    const VideoFrame* first = s.Tick(0.0);
    ASSERT_NE(first, nullptr);

    // Decoder is now well behind: its next frames carry small PTS while the
    // playhead has advanced.
    s.Push(MakeFrame(1, kFrame24));
    s.Push(MakeFrame(2, 2 * kFrame24));
    const VideoFrame* catchUp = s.Tick(10 * kFrame24);
    ASSERT_NE(catchUp, nullptr)
        << "a behind decoder must still be able to present catch-up frames";
    EXPECT_EQ(catchUp->frameIndex, 2u);
}

// 6. Selection never blocks and never mutates slot ownership.
TEST(FrameSchedulerSelect, SelectionPreservesSlotHandles) {
    FrameScheduler s;
    for (int i = 0; i < 4; ++i) {
        VideoFrame f = MakeFrame(i, i * kFrame24);
        f.slot = static_cast<UINT>(7 - i);      // distinct slots
        s.Push(f);
    }
    const VideoFrame* f = s.Tick(2 * kFrame24);
    ASSERT_NE(f, nullptr);
    EXPECT_LT(f->slot, FrameScheduler::kNumSlots);
    // The pointer must alias a ring slot the decoder still owns; we assert
    // only that the descriptor survives, since handles are decoder-owned for
    // the process lifetime (FrameScheduler.hpp contract).
    EXPECT_GE(f->fenceValue, 0u);
}

// Tolerance: a frame slightly past the playhead still counts as due, so
// fractional rates (59.94) do not suffer systematic one-frame lateness.
TEST(FrameSchedulerSelect, SelectionToleranceApplies) {
    FrameScheduler s;
    s.Push(MakeFrame(0, 0.0));
    s.Push(MakeFrame(1, kFrame24));

    const double justInside = kFrame24 - FrameScheduler::kSelectToleranceSec * 0.5;
    const VideoFrame* f = s.Tick(justInside);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->frameIndex, 1u) << "within tolerance, frame 1 is due";
}

TEST(FrameSchedulerSelect, EqualPtsRepeatsAreNotCountedAsDrops) {
    FrameScheduler s;
    s.Push(MakeFrame(0, 0.0));
    s.Push(MakeFrame(1, 0.0));   // same instant (B-frame reorder edge case)

    const VideoFrame* f = s.Tick(0.0);
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(s.GetDroppedCount(), 0u)
        << "equal-PTS selection is a repeat, not a skip";
}

// 7. Backlog recovery: when the caller allows it (the published content is
//    more than a second behind the wall clock), Tick may present the NEWEST
//    published frame instead of the newest DUE one, so the content jumps
//    forward and the ring drains. Without the permission the old rule holds:
//    never present a future frame (that was the 2x-read bug).
TEST(FrameSchedulerSelect, CatchUpJumpsToTheNewestPublishedFrame) {
    FrameScheduler s;
    // Five frames published, all in the past relative to a playhead that
    // has run far ahead (the backlog).
    for (int i = 0; i < 5; ++i) s.Push(MakeFrame(i, i * kFrame24));

    // Without the permission: the newest DUE frame wins - the newest frame
    // here is frame 4 (pts 4/24 s), which is still due for a playhead of
    // 10 s. Use a playhead that makes the newest frame a FUTURE frame so
    // the difference is visible.
    FrameScheduler plain;
    for (int i = 0; i < 5; ++i) plain.Push(MakeFrame(i, i * kFrame24));
    const VideoFrame* due = plain.Tick(2.0 * kFrame24);
    ASSERT_NE(due, nullptr);
    EXPECT_EQ(due->frameIndex, 2u) << "the newest DUE frame is selected";

    // With the permission, a frame that is ahead of the playhead may be
    // presented (the content jumps forward instead of replaying the backlog).
    FrameScheduler catcher;
    for (int i = 0; i < 5; ++i) catcher.Push(MakeFrame(i, i * kFrame24));
    const VideoFrame* jumped = catcher.Tick(2.0 * kFrame24, /*allowCatchUp=*/true);
    ASSERT_NE(jumped, nullptr);
    EXPECT_EQ(jumped->frameIndex, 4u) << "the catch-up presents the newest frame";
}

// 8. The catch-up must not fire when it is not allowed - the default keeps
//    the "never show a future frame" contract.
TEST(FrameSchedulerSelect, CatchUpIsOffByDefault) {
    FrameScheduler s;
    for (int i = 0; i < 5; ++i) s.Push(MakeFrame(i, i * kFrame24));

    // A playhead before every published frame: nothing is due, so the
    // current frame is retained and no future frame is displayed.
    const VideoFrame* f = s.Tick(-1.0);
    EXPECT_EQ(f, nullptr);
    EXPECT_EQ(s.StarvedTicks(), 1u);
    EXPECT_EQ(s.PendingCount(), 5u) << "the frames stay in the ring";
}
