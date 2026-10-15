#pragma once

// P0-2 content-rate check: seconds of CONTENT advanced per second of wall
// clock, computed from presented PTS. 1.0 = realtime.
//
// WHY THIS EXISTS: position reported 1.000x and effFps reported the source
// rate while displayed content actually ran at ~2x (2026-10-15 scheduler
// defect). Every existing speed-like metric derives from the playhead or
// from present cadence, so none of them can detect content running at the
// wrong rate. This meter is derived from the content itself: it sums the
// PTS deltas of frames that were actually presented and divides by wall
// time, which is exactly the quantity the user sees.
//
// Feed every presented frame's PTS in presentation order from the render
// thread; Fold() from the telemetry thread (caller serialises). Fold()
// returns the rate over the folded window and rebases it. Returns the last
// measured rate until the first full window elapses (0.0 before that).
//
// Discontinuities are explicit, because they must never read as a rate
// excursion:
//  - loop wrap (backward jump that unrolls through the known duration into
//    (0, kMaxJumpSec]) -> unrolled and accumulated normally;
//  - backward seek (anything else negative) -> discarded, window rebased;
//  - forward jump > kMaxJumpSec (explicit seek) -> discarded, window rebased.
// Pause/freeze simply stops accumulation, so the next fold reads low -
// that is honest: frozen content IS a rate of 0.
namespace rex::wallpaper {

class ContentRateMeter {
public:
    // Window must exceed the 5 s telemetry sampler interval so two samplers
    // cannot straddle the same partial window and disagree (same reasoning
    // as the drop-rate window in VideoPlaybackController::GetTelemetry).
    static constexpr double kWindowSec = 10.0;
    // Larger than any single legitimate frame step (1.0 s covers 1 fps
    // content) and smaller than any plausible seek (>=1 s jumps are always
    // explicit seeks in REX - playback never skips content forward).
    static constexpr double kMaxJumpSec = 1.0;

    void Reset() {
        m_hasPts   = false;
        m_armed    = false;
        m_lastPts  = 0.0;
        m_clock    = 0.0;
        m_winClock = 0.0;
        m_winWall  = 0.0;
        m_rate     = 0.0;
    }

    void OnPresented(double pts, double durationSec, double nowSec) {
        if (!m_hasPts) {
            m_lastPts = pts;
            m_hasPts  = true;
            Rebase(nowSec);
            return;
        }
        double d      = pts - m_lastPts;
        bool   skip   = false;
        if (d < 0.0) {
            // Loop wrap: the decoder wraps PTS at duration. Unroll only when
            // the result is a single-frame-sized step; a bigger backward
            // unroll means an explicit seek backwards.
            const double unrolled =
                durationSec > 0.0 ? durationSec - m_lastPts + pts : -1.0;
            if (unrolled > 0.0 && unrolled <= kMaxJumpSec) {
                d = unrolled;
            } else {
                skip = true;
            }
        } else if (d > kMaxJumpSec) {
            skip = true;
        }
        if (skip) {
            m_lastPts = pts;
            Rebase(nowSec);  // rebase so the jump never skews a window
            return;
        }
        m_clock += d;
        m_lastPts = pts;
    }

    double Fold(double nowSec) {
        if (!m_armed) return m_rate;
        const double dt = nowSec - m_winWall;
        if (dt < kWindowSec) return m_rate;
        m_rate = (m_clock - m_winClock) / dt;
        Rebase(nowSec);
        return m_rate;
    }

    double LastRate() const { return m_rate; }

private:
    void Rebase(double nowSec) {
        m_winClock = m_clock;
        m_winWall  = nowSec;
        m_armed    = true;
    }

    bool   m_hasPts   = false;
    bool   m_armed    = false;
    double m_lastPts  = 0.0;
    double m_clock    = 0.0;  // cumulative content seconds (wrap-unrolled)
    double m_winClock = 0.0;  // m_clock at window start
    double m_winWall  = 0.0;  // wall seconds at window start
    double m_rate     = 0.0;
};

}  // namespace rex::wallpaper
