#pragma once

// ─── Unified playback arbitration (P1) ──────────────────────────────────────
//
// Thin orchestration over the automatic suspend sources (fullscreen app,
// session lock, console display off, battery saver / critical battery).
// Behavior-preserving extraction of the render-loop pause/resume rule that
// used to live inline in Engine::RenderLoop:
//
//   - the effective pause is fullscreen OR suspend (any source pauses);
//   - resume fires only for OUR pauses (m_autoPaused) — clearing an
//     automatic pause never overrides a user's manual Pause/Play/Stop,
//     which clear m_autoPaused on the command thread.
//
// Pure + header-only so the whole truth table is unit-testable without an
// engine (mirrors PowerPolicy.hpp / PowerPauseRuleTest).

namespace rex::wallpaper {

/// Window-style bits we reason about, so this header stays free of windows.h.
/// Values match the Win32 constants.
inline constexpr long kWsCaption = 0x00C00000L;
inline constexpr long kWsPopup   = 0x80000000L;
inline constexpr long kWsThickFrame = 0x00040000L;

/// Decides whether a window that COVERS the whole monitor is *genuinely*
/// fullscreen, as opposed to an ordinary window that was maximized.
///
/// Why this exists: the previous check was purely geometric (window rect within
/// 2 px of the monitor rect). With the Windows taskbar set to auto-hide, a
/// MAXIMIZED window really is 3840x2160, so every maximized app froze the
/// wallpaper. Proven live 2026-11: a maximized Windows Terminal
/// (class CASCADIA_HOSTING_WINDOW_CLASS, rect 3840x2160) triggered
/// "Auto-pause applied (fullscreen=true, suspend=false)" and pinned playback at
/// position 2.4 s.
///
/// A maximized normal window keeps WS_CAPTION and is not WS_POPUP, and
/// IsZoomed() reports true. Genuine fullscreen (games, video players, borderless
/// exclusive) either drops the caption or uses WS_POPUP, and is not "zoomed".
inline bool IsTrueFullscreenWindow(long style, bool isZoomed) {
    if (isZoomed) return false;
    const bool hasCaption = (style & kWsCaption) != 0;
    const bool isPopup    = (style & kWsPopup) != 0;
    // Caption + not popup == ordinary maximized/restored app window.
    if (hasCaption && !isPopup) return false;
    return true;
}

enum class PlaybackArbitration { None, AutoPause, AutoResume };

struct PlaybackInputs {
    bool fullScreenPaused = false;  // serviced source flags (post-exchange)
    bool suspPaused = false;
    bool autoPaused = false;        // we paused it ourselves
    bool running = false;           // state == Running
    bool paused = false;            // state == Paused
};

inline PlaybackArbitration EvaluatePlayback(const PlaybackInputs& in) {
    const bool wantPaused = in.fullScreenPaused || in.suspPaused;
    if (wantPaused && !in.autoPaused && in.running) {
        return PlaybackArbitration::AutoPause;
    }
    if (!wantPaused && in.autoPaused && in.paused) {
        return PlaybackArbitration::AutoResume;
    }
    return PlaybackArbitration::None;
}

}  // namespace rex::wallpaper
