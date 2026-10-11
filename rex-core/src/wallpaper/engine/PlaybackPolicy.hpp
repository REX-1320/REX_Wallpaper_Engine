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
