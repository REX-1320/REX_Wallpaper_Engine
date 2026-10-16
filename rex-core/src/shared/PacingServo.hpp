#pragma once

// ─── PacingServo ────────────────────────────────────────────────────────────
//
// Closed-loop correction of REX's render-loop pacing.
//
// ── Why this exists (all measured — docs/bugs/BASELINE-2026-10-15-phaseA.md) ──
//
// `PaceLoop` paces open-loop: `deadline = frameStart + interval` on a fixed grid
// with no feedback. Measured over 10 runs per clip at 3840x2160/160 Hz, the
// achieved interval sits a constant ~+0.30-0.33 ms ABOVE nominal:
//
//   obanai 4K H.264 60fps : 16.998 ms vs 16.6667 nominal (+0.3313) -> 58.80/60 fps
//   Aari    4K AV1  24fps : 41.970 ms vs 41.6667 nominal (+0.3033) -> 23.77/24 fps
//
// A *constant* per-frame overshoot predicts those deficits to within 0.05% /
// 0.25%, which identifies it as a per-sleep wake-up overshoot rather than a
// cadence or rounding error. This servo cancels it closed-loop.
//
// ── Why it controls RATE and not absolute submit phase ──────────────────────
//
// The milestone's original sketch was `phaseError = (deadline - lastSync) mod
// refresh - midWindow`. That was measured to be the wrong control variable on this
// hardware, and implementing it would have made things worse. In
// `deadline = frameStart + interval + corr` the next frame starts *at* the
// deadline, so `achievedInterval = interval + corr`: phase and rate are the SAME
// knob, not two independent ones.
//
// At 160 Hz no common source rate divides evenly (24->6.667, 30->5.333,
// 60->2.667, 120->1.333 v-blanks per frame), so the submit phase necessarily
// cycles and its mean is an arbitrary function of where that cycle started.
// Driving the mean to mid-window therefore *is* a rate change:
//
//   24 fps @160 Hz : mean phase error -1.042 ms -> +1.042 ms corr
//                   -> 23.415 fps  =  -2.44 % rate error
//   60 fps @160 Hz : mean phase error -1.042 ms -> +1.042 ms corr
//                   -> 56.471 fps  =  -5.88 % rate error
//
// Both are worse than the +2.00 % deficit the servo exists to remove, and the
// "correct" phase is arbitrary to begin with. So the control variable is the
// measured interval error, which is unambiguous: cancel the overshoot, hold the
// rate, and leave the v-blank phase wherever the content cadence puts it.
// `PhaseErrorMs` is kept as a *diagnostic* (it validates the measured v-blank
// lattice and feeds telemetry), not as the control signal.
//
// ── Phase reference ──
//
// Deliberately the DXGI presentation scheduler, not DWM. With REX playing,
// `DwmGetCompositionTimingInfo` reports a 138.585 Hz composition cadence against
// this 160 Hz panel (a 15.45% period error) with `qpcVBlank == qpcCompose` on
// every sample, i.e. DWM runs no distinct composition pass for this surface.
//
// Pure header, no device, no clock. Every quantity is an argument so the policy
// is unit-testable without a display, which is the whole point given this
// project's history of convincing-but-wrong readings of an unvalidated number.

#include <cmath>

namespace rex::wallpaper {

class PacingServo {
public:
    /// Tuning.
    ///
    /// `proportional` handles transients; `integral` carries the DC gain so a
    /// steady bias is cancelled exactly. `integralLeak` is what makes an
    /// integrator safe here — see the design note on `Step`.
    struct Config {
        double proportional = 2.0;   ///< gain on the low-passed interval error
        double integral     = 0.02;  ///< gain feeding the accumulator
        double integralLeak = 5e-4;  ///< per-frame decay; 1/leak = 2000 frames
        double filterAlpha  = 0.08;  ///< lag on the interval error
        double maxCorrFrac  = 0.10;  ///< clamp, fraction of the interval
    };

    void Configure(const Config& c) { m_cfg = c; }
    void Reset() { m_filtered = 0.0; m_integral = 0.0; m_primed = false; }

    /// Phase of `deadlineQpc` within the display v-blank lattice, in ms,
    /// relative to mid-window. Positive means LATE. Range [-P/2, +P/2).
    ///
    /// Diagnostic only, per the header note. Static and pure so the wrapping
    /// (including the common case of a deadline BEFORE the last sync) is
    /// unit-tested rather than trusted.
    static double PhaseErrorMs(double deadlineQpc, double syncQpc,
                               double periodTicks, double periodMs) {
        if (periodTicks <= 0.0 || periodMs <= 0.0) return 0.0;
        double phase = std::fmod(deadlineQpc - syncQpc, periodTicks);
        if (phase < 0.0) phase += periodTicks;
        return (phase / periodTicks) * periodMs - (0.5 * periodMs);
    }

    /// Advance the loop by one frame.
    ///
    /// @param intervalErrorMs measured interval MINUS nominal interval. Positive
    ///        means frames are arriving LATE (the loop is running slow), which is
    ///        the sign of the measured +0.30-0.33 ms overshoot.
    /// @param intervalMs     nominal interval; sets the correction clamp.
    /// @return correction in ms to ADD to this frame's deadline. Negative pulls
    ///         the deadline earlier, shrinking the achieved interval. Always
    ///         within +/- `maxCorrFrac * intervalMs`.
    ///
    /// Design note: PI with a proportional path for transients and a SLOW LEAKY
    /// integrator for DC gain. Three designs were implemented and measured first:
    ///
    ///  1. PI with an unbounded integral. The accumulator is a free random walk
    ///     under the +/-0.25 ms jitter the baseline actually shows (FrameTime
    ///     p95-p50 = 0.230 ms): it drifted to an uncorrelated +/-1.5 ms offset
    ///     over 20k frames, a ~9% rate error - larger than the +2% bias it exists
    ///     to remove.
    ///  2. Low-passing the error before integrating. Does not rescue it: the
    ///     filtered signal is strongly autocorrelated (rho = 1-alpha = 0.92), so
    ///     the walk merely slows from std 0.408 to 0.295 ms. Measured, not assumed.
    ///  3. Proportional only, no integrator. Clean, but its DC gain is
    ///     K/(1+K), so it can never fully cancel: K=2 leaves a third of the
    ///     bias (measured residual +0.11 ms at 60 fps).
    ///
    /// The leak is the piece that makes (1) safe without giving up DC gain. With
    /// `integralLeak = 5e-4` the accumulator's steady gain is `ki/leak = 40`, so
    /// the loop cancels a constant bias exactly, while its noise variance is
    /// `ki^2 * var(filtered) / (1 - (1-leak)^2)` -- about 0.019 ms at the real
    /// jitter amplitude, i.e. 0.11% of rate. Leak also caps the wind-up of a long
    /// excursion, so a decode stall cannot bank unbounded credit. The cost is a
    /// 2000-frame (33 s at 60 fps) time constant on the DC term, which is the
    /// right trade for correcting a steady bias: the proportional path still
    /// reacts within a few frames.
    double Step(double intervalErrorMs, double intervalMs) {
        if (!(intervalMs > 0.0)) return 0.0;
        const double limit = m_cfg.maxCorrFrac * intervalMs;
        const double intLimit = limit;  // keep the integrator inside the clamp

        if (!m_primed) {
            // Seed with the first real reading but apply nothing this frame: the
            // interval that produced this error is already spent.
            m_filtered = intervalErrorMs;
            m_primed = true;
            return 0.0;
        }

        m_filtered += m_cfg.filterAlpha * (intervalErrorMs - m_filtered);

        // Conditional integration: only bank error while the proportional term
        // alone is still inside the clamp, so a long excursion cannot build
        // credit that would then overshoot on recovery.
        const double proportional = m_cfg.proportional * m_filtered;
        if (std::fabs(proportional) < limit) {
            m_integral += m_cfg.integral * m_filtered;
        }
        m_integral *= (1.0 - m_cfg.integralLeak);
        m_integral = Clamp(m_integral, -intLimit, intLimit);

        // A late (positive) interval error is corrected by going earlier.
        return Clamp(-(proportional + m_integral), -limit, limit);
    }

    /// Diagnostics for telemetry.
    double FilteredErrorMs() const { return m_primed ? m_filtered : 0.0; }
    double IntegralMs() const { return m_integral; }
    bool   Primed() const { return m_primed; }

private:
    static double Clamp(double v, double lo, double hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    Config m_cfg{};
    double m_filtered = 0.0;
    double m_integral = 0.0;
    bool   m_primed = false;
};

} // namespace rex::wallpaper
