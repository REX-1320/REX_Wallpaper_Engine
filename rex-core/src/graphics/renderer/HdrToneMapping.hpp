#pragma once

// ─── HDR -> SDR tone mapping (mpv / libplacebo parity) ────────────────────
//
// A port of libplacebo v7.360.0's tone-mapping path - the build shipped in
// mpv 0.41.0, the reference player for this engine's video output.
//
// Upstream sources (github.com/haasn/libplacebo, tag v7.360.0):
//   src/tone_mapping.c        - st2094_pick_knee(), spline()
//   src/shaders/colorspace.c  - pl_shader_color_map_ex() (the PQ<->IPT plumbing)
//   src/lp_colorspace.c       - pl_color_space_nominal_luma_ex() (the anchors)
//   src/lp_sh_colorspace.c    - pl_shader_detect_peak() (the peak detector)
//
// The engine's previous path was a Hable filmic fit (VLC-shaped) which crushed
// shadows: 27 % of an HDR frame below code 4 against mpv's 9 %. This port
// reproduces mpv's measured transfer function to within 0.1 code/255 over a
// 24-band PQ ramp (0.01 .. 4000 cd/m²) - see the unit test in
// tests/wallpaper/test_color_pipeline.cpp.
//
// Two halves, kept in lockstep:
//   * the CPU side here (anchors, knee, spline) computes the five constants
//     the shader needs, plus a reference Evaluate() for the test;
//   * the shader (D3D12Renderer.cpp) evaluates the same polynomial per pixel.
//
// Everything below is in PQ space (SMPTE ST 2084 codes), exactly like
// libplacebo: the curve is solved in PQ and the result is decoded to linear
// light afterwards.

#include <cmath>

namespace rex::wallpaper::tone {

// ── SMPTE ST 2084 (PQ) ───────────────────────────────────────────────────

inline constexpr float kPqM1 = 0.1593017578125f;  // 2610/16384
inline constexpr float kPqM2 = 78.84375f;          // 2523/32
inline constexpr float kPqC1 = 0.8359375f;         // 3424/4096
inline constexpr float kPqC2 = 18.8515625f;        // 2413/128
inline constexpr float kPqC3 = 18.6875f;          // 2392/128

/// libplacebo PL_COLOR_SDR_WHITE - reference white of an SDR signal.
inline constexpr float kSdrWhiteNits = 203.0f;
/// libplacebo PL_COLOR_SDR_CONTRAST - assumed contrast of an unknown SDR
/// display; fixes the target black at 203/1000 cd/m².
inline constexpr float kSdrContrast = 1000.0f;
/// libplacebo PL_COLOR_HDR_BLACK - black floor for HDR sources without a
/// tagged minimum luminance.
inline constexpr float kHdrBlackNits = 1e-6f;

/// PQ inverse EOTF: absolute nits -> PQ code.
inline float PqEncode(float nits) {
    const float y = nits > 0.0f ? nits / 10000.0f : 0.0f;
    const float p = std::pow(y, kPqM1);
    return std::pow((kPqC1 + kPqC2 * p) / (1.0f + kPqC3 * p), kPqM2);
}

/// PQ EOTF: PQ code -> absolute nits.
inline float PqDecode(float pq) {
    const float p = std::pow(pq > 0.0f ? pq : 0.0f, 1.0f / kPqM2);
    const float num = p - kPqC1;
    if (num <= 0.0f) return 0.0f;
    const float den = kPqC2 - kPqC3 * p;
    if (den <= 0.0f) return 0.0f;
    return 10000.0f * std::pow(num / den, 1.0f / kPqM1);
}

/// PQ code of the target black (0.203 cd/m²) - the anchor libplacebo's
/// curve maps the source black to.
inline constexpr float kTargetBlackPq = 0.08332753f;   // PqEncode(0.203)
/// PQ code of the SDR reference white (203 cd/m²) - the curve's output max.
inline constexpr float kTargetWhitePq = 0.58068888f;   // PqEncode(203)
/// The same target black in libplacebo's NORM scaling (1.0 == 203 cd/m²):
/// 0.203 cd/m² / 203 cd/m². This is the value the black-scaled delinearize
/// subtracts, so the target black lands on output code 0.
inline constexpr float kTargetBlackNorm = 0.001f;

// ── Small helpers matching libplacebo's PL_MIX / pl_smoothstep ───────────

inline float Mix(float a, float b, float t) { return a + (b - a) * t; }

inline float Clamp(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

/// libplacebo pl_smoothstep (edge0 may exceed edge1 - the knee code relies
/// on that: smoothstep(0.8, 0.4, x) falls from 1 to 0 as x rises).
inline float Smoothstep(float edge0, float edge1, float x) {
    if (edge0 == edge1) return x >= edge0 ? 1.0f : 0.0f;
    float t = Clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// ── The tone curve ───────────────────────────────────────────────────────

/// libplacebo's "spline" tone map, solved in PQ space.
///
/// The curve is a single pivot: a quadratic toe below the pivot (Pa) and a
/// cubic shoulder above it (Qa/Qb), both anchored so that
///   curve(input_min)  == output_min
///   curve(input_max)  == output_max
///   d curve / d x     == slope at the pivot.
struct ToneMapCurve {
    // ---- the five values the shader needs ----
    float srcPivot = 0.0f;  // PQ - source knee
    float dstPivot = 0.0f;  // PQ - destination knee
    float slope    = 1.0f;  // dP/dx at the pivot
    float pa       = 0.0f;  // toe coefficient
    float inMax    = 0.0f;  // PQ - the (clamped) source peak

    // ---- derived; the shader recomputes qa/qb from inMax, the CPU
    //      reference needs them directly ----
    float qa = 0.0f;
    float qb = 0.0f;

    // ---- anchors (PQ) ----
    float inMin  = 0.0f;
    float inAvg  = 0.0f;
    float outMin = 0.0f;
    float outMax = 0.0f;

    /// The curve itself - the exact polynomial the shader evaluates.
    ///
    /// The input is clamped to the curve's domain, exactly like libplacebo's
    /// LUT sampling does (the LUT is sampled with CLAMP_TO_EDGE, so a value
    /// outside [inMin, inMax] returns the curve's end point).
    float Evaluate(float pq) const {
        const float x = Clamp(pq, inMin, inMax) - srcPivot;
        const float y = x > 0.0f ? ((qa * x + qb) * x + slope) * x
                                 : (pa * x + slope) * x;
        return y + dstPivot;
    }

    /// Output signal (0..1) for an input PQ code, including libplacebo's
    /// black-scaled delinearize: the target black (0.203 cd/m²) becomes 0
    /// and the SDR reference white (203 cd/m²) becomes 1.
    float ToDisplay(float pq) const {
        const float nits = PqDecode(Evaluate(pq));
        const float norm = (nits / kSdrWhiteNits - kTargetBlackNorm) /
                           (1.0f - kTargetBlackNorm);
        return Clamp(norm, 0.0f, 1.0f);
    }
};

/// Builds libplacebo's spline curve for a measured source peak / average.
///
/// `maxPq` and `avgPq` are PQ codes as measured by the peak detector
/// (mean / max of the PQ-encoded luma, black pixels excluded).
inline ToneMapCurve BuildSplineCurve(float maxPq, float avgPq) {
    // libplacebo PL_TONE_MAP_CONSTANTS (v7.360.0).
    constexpr float kKneeAdaptation = 0.4f;
    constexpr float kKneeMinimum     = 0.1f;
    constexpr float kKneeMaximum     = 0.8f;
    constexpr float kKneeDefault     = 0.4f;
    constexpr float kSlopeTuning     = 1.5f;
    constexpr float kSlopeOffset     = 0.2f;
    constexpr float kSplineContrast  = 0.5f;

    ToneMapCurve c;
    c.inMin  = PqEncode(kHdrBlackNits);
    c.outMin = kTargetBlackPq;
    c.outMax = kTargetWhitePq;

    // pl_tone_map_params_infer(): "constrain the input peak to be no less
    // than 100 nits" (PQ of 100 nits, capped by the output peak).
    const float minInputPeak = PqEncode(100.0f);
    c.inMax = Clamp(maxPq, minInputPeak, 1.0f);
    c.inAvg = Clamp(avgPq, c.inMin, c.inMax);

    // st2094_pick_knee()
    const float srcKneeMin = Mix(c.inMin, c.inMax, kKneeMinimum);
    const float srcKneeMax = Mix(c.inMin, c.inMax, kKneeMaximum);
    const float dstKneeMin = Mix(c.outMin, c.outMax, kKneeMinimum);
    const float dstKneeMax = Mix(c.outMin, c.outMax, kKneeMaximum);

    // PL_DEF(src_avg, ...): a measured average wins; otherwise the default
    // knee at 40 % of the source range.
    float srcKnee = c.inAvg != 0.0f
                        ? c.inAvg
                        : Mix(c.inMin, c.inMax, kKneeDefault);
    srcKnee = Clamp(srcKnee, srcKneeMin, srcKneeMax);

    const float target = (srcKnee - c.inMin) / (c.inMax - c.inMin);
    const float adapted = Mix(c.outMin, c.outMax, target);
    const float tuning = 1.0f - Smoothstep(kKneeMaximum, kKneeDefault, target) *
                                   Smoothstep(kKneeMinimum, kKneeDefault, target);
    const float adaptation = Mix(kKneeAdaptation, 1.0f, tuning);
    float dstKnee = Mix(srcKnee, adapted, adaptation);
    dstKnee = Clamp(dstKnee, dstKneeMin, dstKneeMax);

    c.srcPivot = srcKnee;
    c.dstPivot = dstKnee;

    // spline(): slope at the pivot, tuned by the peak ratio.
    float slope = (dstKnee - c.outMin) / (srcKnee - c.inMin);
    float ratio = c.inMax / c.outMax - 1.0f;
    ratio = Clamp(kSlopeTuning * ratio, kSlopeOffset, 1.0f + kSlopeOffset);
    slope = std::pow(slope, (1.0f - kSplineContrast) * ratio);
    c.slope = slope;

    // Normalise around the pivot, then solve the toe (P, order 2) and the
    // shoulder (Q, order 3).
    const float inMinN  = c.inMin - srcKnee;
    const float inMaxN  = c.inMax - srcKnee;
    const float outMinN = c.outMin - dstKnee;
    const float outMaxN = c.outMax - dstKnee;

    c.pa = (outMinN - slope * inMinN) / (inMinN * inMinN);
    const float t = 2.0f * inMaxN * inMaxN;
    c.qa = (slope * inMaxN - outMaxN) / (inMaxN * t);
    c.qb = -3.0f * (slope * inMaxN - outMaxN) / t;

    return c;
}

// ── Peak detection state ─────────────────────────────────────────────────

/// The temporally smoothed source peak / average, exactly as libplacebo keeps
/// it (lp_sh_colorspace.c, update_peak_buf()).
///
/// mpv's defaults feed this: smoothing_period = 20 frames (its
/// "hdr-peak-decay-rate"), scene thresholds 1 % / 3 % in PQ space.
struct PeakState {
    float maxPq = 0.0f;
    float avgPq = 0.0f;
    bool  valid = false;

    /// libplacebo's IIR coefficient for a 20-frame smoothing period.
    static float SmoothingCoefficient() {
        return 1.0f - std::exp(-1.0f / 20.0f);
    }

    /// Folds a fresh measurement into the smoothed state.
    ///
    /// `bias` is libplacebo's frame_wg_active/frame_wg_count factor (the
    /// fraction of the frame that carried signal); 1.0 for a full frame.
    void Update(float measuredMaxPq, float measuredAvgPq, float bias = 1.0f) {
        if (!valid) {
            maxPq = measuredMaxPq;
            avgPq = measuredAvgPq;
            valid = true;
            return;
        }
        const float coeff = SmoothingCoefficient();
        maxPq += coeff * (measuredMaxPq - maxPq);
        avgPq += coeff * (measuredAvgPq - avgPq);

        // Scene-change hysteresis: a big jump in the frame average snaps the
        // smoothed values to the measurement instead of ramping.
        const float delta = bias * std::fabs(measuredAvgPq - avgPq);
        const float mix = Smoothstep(0.01f, 0.03f, delta);
        maxPq = Mix(maxPq, measuredMaxPq, mix);
        avgPq = Mix(avgPq, measuredAvgPq, mix);
    }

    void Reset() {
        maxPq = 0.0f;
        avgPq = 0.0f;
        valid = false;
    }
};

}  // namespace rex::wallpaper::tone
