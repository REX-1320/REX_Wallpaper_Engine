// PacingServo — closed-loop correction of the render loop's pacing bias.
//
// Pure, so it is testable with no display, no swap chain and no GPU. That matters
// here specifically: this class exists because REX's submit-side telemetry could
// not see the error it corrects, and this project's history is a run of
// convincing-but-wrong readings of an unvalidated number (wall_research.md P10,
// and the cadence sampler misread in P11). A pure, unit-tested policy cannot be
// misread that way.
//
// The behaviours pinned here are the ones the live acceptance depends on:
//   - sign convention (a late interval must be pulled EARLIER),
//   - convergence on the exact bias the Phase A baseline measured,
//   - the +/-10% clamp (never destabilise the grid),
//   - leak-bounded wind-up on a long excursion,
//   - jitter rejection at the amplitude the baseline actually shows,
//   - behaviour across the whole 24/30/60/120 fps range the engine produces.
//
// The closed-loop model below is the honest one. In `deadline = frameStart +
// interval + corr` the next frame starts AT the deadline, so the achieved
// interval becomes `interval + corr` — phase and rate are the same knob. A test
// that holds the error constant while ignoring the correction would describe a
// servo pinned at its clamp, not a servo in a running engine.

#include <gtest/gtest.h>

#include <shared/PacingServo.hpp>

using rex::wallpaper::PacingServo;

namespace {

// 160 Hz panel: 6.25 ms period. Raw QPC is 10 MHz on this box.
constexpr double kPeriodMs = 6.25;
constexpr double kPeriodTicks = kPeriodMs * 10000.0;  // 62500 ticks @ 10 MHz

// The integrator's time constant is 1/integralLeak = 2000 frames, so DC
// convergence needs a few multiples of that. A wallpaper runs for hours, so this
// is a short horizon in practice (500 frames = 8 s at 60 fps).
constexpr int kSettleFrames = 6000;

/// Runs the real closed loop: the correction we apply feeds straight back into
/// the achieved interval, exactly as it does in PaceLoop.
/// @return the steady-state correction, which should approach -bias.
double Settle(double biasMs, double intervalMs, int frames = kSettleFrames,
              double* integralOut = nullptr) {
    PacingServo s;
    s.Step(0.0, intervalMs);
    double achieved = intervalMs + biasMs;
    double corr = 0.0;
    for (int i = 0; i < frames; ++i) {
        corr = s.Step(achieved - intervalMs, intervalMs);
        achieved = intervalMs + biasMs + corr;
    }
    if (integralOut) *integralOut = s.IntegralMs();
    return corr;
}

}  // namespace

// ─── PhaseErrorMs (diagnostic lattice helper) ───────────────────────────────

TEST(PacingServo, PhaseErrorIsZeroExactlyAtMidWindow) {
    EXPECT_NEAR(PacingServo::PhaseErrorMs(100000.0 + kPeriodTicks / 2.0, 100000.0,
                                          kPeriodTicks, kPeriodMs),
                0.0, 1e-9);
}

TEST(PacingServo, PhaseErrorIsNegativeHalfPeriodAtAVblank) {
    EXPECT_NEAR(PacingServo::PhaseErrorMs(100000.0, 100000.0,
                                          kPeriodTicks, kPeriodMs),
                -kPeriodMs / 2.0, 1e-9);
}

TEST(PacingServo, PhaseErrorWrapsForNegativeDelta) {
    // A deadline BEFORE the last sync is the common case; it must wrap into
    // [0,P) instead of reporting a negative phase.
    EXPECT_NEAR(PacingServo::PhaseErrorMs(100000.0 - kPeriodTicks / 2.0, 100000.0,
                                          kPeriodTicks, kPeriodMs),
                0.0, 1e-9);
}

TEST(PacingServo, PhaseErrorStaysWithinHalfPeriod) {
    for (int i = -50; i <= 50; ++i) {
        const double err = PacingServo::PhaseErrorMs(
            100000.0 + i * kPeriodTicks / 50.0, 100000.0, kPeriodTicks, kPeriodMs);
        EXPECT_GE(err, -kPeriodMs / 2.0 - 1e-9);
        EXPECT_LT(err, kPeriodMs / 2.0 + 1e-9);
    }
}

TEST(PacingServo, PhaseErrorIsZeroWithoutAPeriod) {
    EXPECT_DOUBLE_EQ(PacingServo::PhaseErrorMs(100.0, 0.0, 0.0, kPeriodMs), 0.0);
    EXPECT_DOUBLE_EQ(PacingServo::PhaseErrorMs(100.0, 0.0, kPeriodTicks, 0.0), 0.0);
}

// ─── Step: sign and bounds ──────────────────────────────────────────────────

TEST(PacingServo, FirstStepIsPrimingOnlyAndAppliesNothing) {
    PacingServo s;
    EXPECT_FALSE(s.Primed());
    EXPECT_DOUBLE_EQ(s.Step(0.33, 16.667), 0.0);
    EXPECT_TRUE(s.Primed());
}

TEST(PacingServo, ALateIntervalIsPulledEarlier) {
    // Sign convention: positive error (late) must yield a NEGATIVE correction,
    // because the correction is added to the deadline.
    PacingServo s;
    s.Step(0.0, 16.667);
    double corr = 0.0;
    for (int i = 0; i < 200; ++i) corr = s.Step(+1.0, 16.667);
    EXPECT_LT(corr, 0.0);
}

TEST(PacingServo, AnEarlyIntervalIsPushedLater) {
    PacingServo s;
    s.Step(0.0, 16.667);
    double corr = 0.0;
    for (int i = 0; i < 200; ++i) corr = s.Step(-1.0, 16.667);
    EXPECT_GT(corr, 0.0);
}

TEST(PacingServo, CorrectionNeverExceedsTenPercentOfInterval) {
    for (double fps : {24.0, 30.0, 60.0, 120.0}) {
        const double interval = 1000.0 / fps;
        PacingServo s;
        s.Step(0.0, interval);
        for (int i = 0; i < 3000; ++i) {
            const double corr = s.Step(100.0, interval);  // absurd error
            EXPECT_LE(corr, 0.10 * interval + 1e-9) << "fps=" << fps;
            EXPECT_GE(corr, -0.10 * interval - 1e-9) << "fps=" << fps;
        }
    }
}

TEST(PacingServo, CorrectionIsZeroWithoutAnInterval) {
    PacingServo s;
    s.Step(0.0, 16.667);
    EXPECT_DOUBLE_EQ(s.Step(0.33, 0.0), 0.0);
}

// ─── Convergence: the real target ───────────────────────────────────────────

TEST(PacingServo, CancelsTheExactBiasThePhaseABaselineMeasured) {
    // obanai +0.3313 ms/frame at 60 fps and Aari +0.3033 ms/frame at 24 fps.
    // DC gain is 1/(1 + 1/leak) in this design, so >95% cancellation is the
    // expected steady state; the point is that it is a factor, not a rounding.
    for (double bias : {0.3033, 0.3313}) {
        for (double fps : {24.0, 60.0}) {
            const double corr = Settle(bias, 1000.0 / fps);
            const double dcGain = (bias + corr) / bias;
            EXPECT_LT(std::fabs(dcGain), 0.05)
                << "bias=" << bias << " fps=" << fps << " dcGain=" << dcGain;
        }
    }
}

TEST(PacingServo, RecoversTheAchievedRateToNominal) {
    // The acceptance criterion in rate terms. The leak bounds the DC gain at
    // 1/(1 + 1/leak) = 0.977, so a residual of about 2.3% of the bias survives —
    // 0.0077 ms. That is 0.03% of rate at 24 fps but 0.09% at 120 fps, where
    // the interval is small, so the bound is relative rather than absolute.
    // Baseline deficits this replaces: -0.77% (24), -0.98% (30), -1.94% (60),
    // -3.95% (120).
    for (double fps : {24.0, 30.0, 60.0, 120.0}) {
        const double interval = 1000.0 / fps;
        const double corr = Settle(0.33, interval);
        const double achievedFps = 1000.0 / (interval + 0.33 + corr);
        EXPECT_LT(std::fabs(achievedFps - fps) / fps, 0.002)
            << "fps=" << fps << " achieved=" << achievedFps;
    }
}

TEST(PacingServo, ZeroBiasProducesNoCorrection) {
    for (double fps : {24.0, 60.0}) {
        EXPECT_NEAR(Settle(0.0, 1000.0 / fps), 0.0, 1e-6) << "fps=" << fps;
    }
}

TEST(PacingServo, HandlesAnEarlyBiasSymmetrically) {
    const double corr = Settle(-0.5, 1000.0 / 60.0);
    EXPECT_NEAR(corr, 0.5, 0.03);
}

TEST(PacingServo, SettlesRatherThanOscillating) {
    // The last 200 frames must be flat, not hunting.
    PacingServo s;
    const double interval = 1000.0 / 60.0;
    s.Step(0.0, interval);
    double achieved = interval + 0.3313;
    double corr = 0.0;
    for (int i = 0; i < 3000; ++i) {
        corr = s.Step(achieved - interval, interval);
        achieved = interval + 0.3313 + corr;
    }
    const double settled = corr;
    for (int i = 0; i < 200; ++i) {
        corr = s.Step(achieved - interval, interval);
        achieved = interval + 0.3313 + corr;
    }
    EXPECT_NEAR(corr, settled, 1e-4);
}

// ─── Noise rejection ────────────────────────────────────────────────────────

TEST(PacingServo, AlternatingJitterDoesNotSustainAnOffset) {
    // Decode jitter makes the interval wobble either way by ~0.25 ms — the
    // amplitude the baseline actually shows (FrameTime p95-p50 = 0.230 ms).
    PacingServo s;
    const double interval = 1000.0 / 60.0;
    s.Step(0.0, interval);
    double mean = 0.0;
    for (int i = 0; i < 20000; ++i) mean += s.Step((i % 2) ? 0.25 : -0.25, interval);
    EXPECT_NEAR(mean / 20000.0, 0.0, 0.01);
}

TEST(PacingServo, RandomJitterDoesNotSustainAnOffset) {
    // This is the property the integral leak exists to guarantee. Measured with an
    // unbounded integrator it drifted to an uncorrelated +/-1.5 ms offset (~9% of
    // rate); the leak must keep the mean negligible over a long run. The bound
    // is 0.05 ms = 0.3% of a 60 fps interval, comfortably inside the -1.94%
    // deficit the servo is here to remove.
    PacingServo s;
    const double interval = 1000.0 / 60.0;
    s.Step(0.0, interval);
    uint32_t seed = 12345;
    double mean = 0.0;
    for (int i = 0; i < 50000; ++i) {
        seed = seed * 1664525u + 1013904223u;
        const double jitter = ((double)(seed >> 8) / 16777216.0 - 0.5) * 0.5;
        mean += s.Step(jitter, interval);
    }
    EXPECT_NEAR(mean / 50000.0, 0.0, 0.05);
}

TEST(PacingServo, JitterSpikesStayBounded) {
    PacingServo s;
    const double interval = 1000.0 / 60.0;
    s.Step(0.0, interval);
    double peak = 0.0;
    for (int i = 0; i < 5000; ++i) {
        const double spike = (i % 500 == 0) ? 5.0 : 0.25;  // periodic hard jitter
        const double c = s.Step(spike, interval);
        if (std::fabs(c) > peak) peak = std::fabs(c);
    }
    EXPECT_LE(peak, 0.10 * interval + 1e-9);
}

// ─── Wind-up ────────────────────────────────────────────────────────────────

TEST(PacingServo, ALongExcursionDoesNotBankCreditThatOvershoots) {
    PacingServo s;
    const double interval = 1000.0 / 60.0;
    s.Step(0.0, interval);
    // 3000 frames of absurd error: a decode stall, DISJOINT, or HDR switch.
    for (int i = 0; i < 3000; ++i) s.Step(100.0, interval);
    // The leak bounds it: the accumulator cannot exceed the correction clamp.
    EXPECT_LE(s.IntegralMs(), interval * 0.10 + 1e-9);
    EXPECT_GE(s.IntegralMs(), -interval * 0.10 - 1e-9);
    // Recovery must start from inside the clamp, not slam past it.
    const double corr = s.Step(0.0, interval);
    EXPECT_LE(corr, interval * 0.10 + 1e-9);
    EXPECT_GE(corr, -interval * 0.10 - 1e-9);
}

TEST(PacingServo, RecoversFromAStallWithoutALongTail) {
    // After a 300-frame stall the achieved interval must come back to nominal.
    PacingServo s;
    const double interval = 1000.0 / 60.0;
    s.Step(0.0, interval);
    double achieved = interval + 50.0;
    for (int i = 0; i < 300; ++i) {
        const double c = s.Step(achieved - interval, interval);
        achieved = interval + 50.0 + c;
    }
    // Stall clears; there is no longer any bias to correct.
    for (int i = 0; i < 2000; ++i) {
        const double c = s.Step(achieved - interval, interval);
        achieved = interval + c;
    }
    EXPECT_NEAR(achieved - interval, 0.0, 0.1);
}

TEST(PacingServo, ResetClearsAllState) {
    PacingServo s;
    s.Step(0.0, 16.667);
    for (int i = 0; i < 200; ++i) s.Step(1.0, 16.667);
    EXPECT_NE(s.IntegralMs(), 0.0);
    s.Reset();
    EXPECT_FALSE(s.Primed());
    EXPECT_DOUBLE_EQ(s.IntegralMs(), 0.0);
    EXPECT_DOUBLE_EQ(s.FilteredErrorMs(), 0.0);
    EXPECT_DOUBLE_EQ(s.Step(0.33, 16.667), 0.0);
}
