// ═══════════════════════════════════════════════════════════════════════════
// P1 regression tests: unified playback arbitration
// ═══════════════════════════════════════════════════════════════════════════
//
// The render loop used to inline the pause/resume rule next to the two
// request channels (fullscreen + suspend). PlaybackPolicy.hpp extracts it
// unchanged: any automatic source pauses; resume fires only for pauses the
// engine applied itself. A user's manual Pause/Play/Stop clears m_autoPaused,
// so clearing an automatic pause must NEVER resume user-paused playback.

#include <gtest/gtest.h>

#include "wallpaper/engine/PlaybackPolicy.hpp"

using namespace rex::wallpaper;

namespace {

PlaybackInputs Base() {
    PlaybackInputs in;
    in.running = true;
    return in;
}

}  // namespace

TEST(PlaybackArbitrationTest, FullscreenPausesRunningPlayback) {
    auto in = Base();
    in.fullScreenPaused = true;
    EXPECT_EQ(EvaluatePlayback(in), PlaybackArbitration::AutoPause);
}

TEST(PlaybackArbitrationTest, SuspendPausesRunningPlayback) {
    auto in = Base();
    in.suspPaused = true;
    EXPECT_EQ(EvaluatePlayback(in), PlaybackArbitration::AutoPause);
}

TEST(PlaybackArbitrationTest, BothSourcesPauseOnce) {
    auto in = Base();
    in.fullScreenPaused = true;
    in.suspPaused = true;
    EXPECT_EQ(EvaluatePlayback(in), PlaybackArbitration::AutoPause);
}

TEST(PlaybackArbitrationTest, NoDoublePauseWhileAutoPaused) {
    auto in = Base();
    in.fullScreenPaused = true;
    in.autoPaused = true;
    EXPECT_EQ(EvaluatePlayback(in), PlaybackArbitration::None);
}

TEST(PlaybackArbitrationTest, UserPausedPlaybackIsNeverAutoPaused) {
    PlaybackInputs in;
    in.fullScreenPaused = true;
    in.autoPaused = false;
    in.running = false;
    in.paused = true;  // user hit Pause: state Paused, not ours
    EXPECT_EQ(EvaluatePlayback(in), PlaybackArbitration::None);
}

TEST(PlaybackArbitrationTest, StoppedEngineNeverAutoPauses) {
    PlaybackInputs in;
    in.fullScreenPaused = true;
    in.suspPaused = true;
    in.running = false;
    in.paused = false;
    EXPECT_EQ(EvaluatePlayback(in), PlaybackArbitration::None);
}

TEST(PlaybackArbitrationTest, ClearingSourcesResumesOnlyOurPause) {
    PlaybackInputs in;
    in.autoPaused = true;
    in.running = false;
    in.paused = true;
    EXPECT_EQ(EvaluatePlayback(in), PlaybackArbitration::AutoResume);
}

TEST(PlaybackArbitrationTest, ClearingSourcesNeverResumesUserPause) {
    // THE user-intent case: user paused manually (autoPaused false);
    // automatic sources clearing must not restart playback.
    PlaybackInputs in;
    in.autoPaused = false;
    in.running = false;
    in.paused = true;
    EXPECT_EQ(EvaluatePlayback(in), PlaybackArbitration::None);
}

TEST(PlaybackArbitrationTest, SteadyRunningNeedsNoDecision) {
    EXPECT_EQ(EvaluatePlayback(Base()), PlaybackArbitration::None);
}

TEST(PlaybackArbitrationTest, TransientAutoPausedWhileRunningNeedsNoDecision) {
    auto in = Base();
    in.autoPaused = true;  // pause applied, state not yet flipped
    EXPECT_EQ(EvaluatePlayback(in), PlaybackArbitration::None);
}

// ═══════════════════════════════════════════════════════════════════════════
// Fullscreen detection: maximized must not count as fullscreen
// ═══════════════════════════════════════════════════════════════════════════
//
// Regression: CheckFullScreenPause decided "fullscreen" purely from window
// coverage. With the taskbar auto-hidden a maximized window is genuinely
// 3840x2160, so a maximized Windows Terminal produced "Auto-pause applied
// (fullscreen=true)" and pinned playback at position 2.4 s. This corrupted four
// of six headless timing measurements during the stutter investigation, so the
// false positive has to be nailed down by test, not by inspection.

// WS_OVERLAPPEDWINDOW = WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_THICKFRAME|
//                       WS_MINIMIZEBOX|WS_MAXIMIZEBOX
constexpr long kOverlapped = 0x00000000L | kWsCaption | kWsThickFrame;

TEST(FullscreenDetection, MaximizedWindowIsNotFullscreen) {
    // IsZoomed() true short-circuits: this is the auto-hidden-taskbar case.
    EXPECT_FALSE(IsTrueFullscreenWindow(kOverlapped, /*isZoomed=*/true));
}

TEST(FullscreenDetection, CaptionedWindowIsNotFullscreen) {
    // Restored (not zoomed) but still a normal app window.
    EXPECT_FALSE(IsTrueFullscreenWindow(kOverlapped, /*isZoomed=*/false));
}

TEST(FullscreenDetection, BorderlessPopupIsFullscreen) {
    // Borderless-fullscreen games / players: WS_POPUP, no caption.
    EXPECT_TRUE(IsTrueFullscreenWindow(kWsPopup, /*isZoomed=*/false));
}

TEST(FullscreenDetection, CaptionlessOverlappedIsFullscreen) {
    // Caption removed but not a popup (some kiosk/embedded hosts).
    EXPECT_TRUE(IsTrueFullscreenWindow(kWsThickFrame, /*isZoomed=*/false));
}

TEST(FullscreenDetection, ZoomedPopupStillNotFullscreen) {
    // IsZoomed wins even for a popup style: a maximized window is still an
    // ordinary window the user can restore, and must not freeze the wallpaper.
    EXPECT_FALSE(IsTrueFullscreenWindow(kWsPopup, /*isZoomed=*/true));
}