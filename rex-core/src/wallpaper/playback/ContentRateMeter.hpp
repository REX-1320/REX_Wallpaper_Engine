#pragma once

// P0-2 content-rate check: seconds of CONTENT advanced per second of wall
// clock, computed from presented PTS. 1.0 = realtime.
//
// WHY THIS EXISTS: position reported 1.000x and effFps reported the source
// rate while displayed content actually ran at ~2x (2026-10-15 scheduler
// defect). Every existing speed-like metric derives from the playhead or
// from present cadence, so none of them can detect content running at the
// wrong rate. This meter is derived from the content itself: it tracks the
// advance of the presented-PTS watermark against wall time, which is
// exactly the quantity the user sees.
//
// Feed every presented frame's PTS in selection order from the render
// thread; Fold() from the telemetry thread (caller serialises). Fold()
// returns the rate over the folded window and rebases it. Returns the last
// measured rate until the first full window elapses (0.0 before that).
//
// Three hard-won rules (measured 2026-10-15 on marvel-spiderman):
//  1. Presented PTS is NOT monotonic: pushes arrive in decode order, so
//     B-frame reorder steps backward by a frame or two constantly. A meter
//     that sums signed deltas (or rebases the window on any backward step)
//     reads ~0.01 while content plays perfectly at 1x. This meter tracks a
//     max watermark instead and ignores backward wobble inside the reorder
//     slack - same treatment the scheduler's IsBackwardJump gives it.
//  2. A single present can legitimately jump seconds forward (freeze
//     recovery presents the first current frame after a stall). Each step's
//     contribution is clamped to kMaxStepSec so a catch-up jump cannot read
//     as a fast-forward burst, while sustained 2x advance (small steps,
//     every frame) still folds to ~2x.
//  3. Wrap/seek (backward beyond slack, forward beyond a jump) starts a new
//     epoch: rebase the window and re-seed the watermark, so the jump never
//     reads as a rate excursion in either direction. The controller resets
//     the meter at every scheduler-Clearing event (Open/Stop/Seek/loop
//     wrap) for the same reason; pause/freeze simply stops accumulation, so
//     the next fold reads low - frozen content IS a rate of 0.
namespace rex::wallpaper {

class ContentRateMeter {
public:
    // Window must exceed the 5 s telemetry sampler interval so two samplers
    // cannot straddle the same partial window and disagree (same reasoning
    // as the drop-rate window in VideoPlaybackController::GetTelemetry).
    static constexpr double kWindowSec = 10.0;
    // Larger than any single legitimate frame step (1.0 s covers 1 fps
    // content) and smaller than any plausible seek: a forward jump past
    // this is always an explicit seek, never playback.
    static constexpr double kMaxJumpSec = 1.0;
    // Bounds one step's contribution (rule 2): a freeze-recovery jump of N
    // seconds counts as 0.5 s of content, never N.
    static constexpr double kMaxStepSec = 0.5;
    // Backward wobble inside this is decode-order B-frame jitter (rule 1).
    // Past it lies wrap/seek (rule 3).
    static constexpr double kReorderSlackSec = 0.5;

    void Reset() {
        m_hasPts   = false;
        m_armed    = false;
        m_max      = 0.0;
        m_clock    = 0.0;
        m_winClock = 0.0;
        m_winWall  = 0.0;
        m_rate     = 0.0;
    }

    void OnPresented(double pts, double /*durationSec*/, double nowSec) {
        if (!m_hasPts) {
            m_max    = pts;
            m_hasPts = true;
            Rebase(nowSec);
            return;
        }
        const double d = pts - m_max;
        if (d > 0.0) {
            if (d > kMaxJumpSec) {
                // Explicit forward seek: new epoch, never a rate spike.
                m_max = pts;
                Rebase(nowSec);
                return;
            }
            m_clock += (d > kMaxStepSec ? kMaxStepSec : d);
            m_max = pts;
        } else if (d < -kReorderSlackSec) {
            // Loop wrap or backward seek: new epoch, never negative.
            m_max = pts;
            Rebase(nowSec);
        }
        // Small backward step: decode-order reorder wobble. Ignored.
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
    double m_max      = 0.0;  // max presented PTS this epoch (watermark)
    double m_clock    = 0.0;  // cumulative clamped content seconds
    double m_winClock = 0.0;  // m_clock at window start
    double m_winWall  = 0.0;  // wall seconds at window start
    double m_rate     = 0.0;
};

}  // namespace rex::wallpaper
