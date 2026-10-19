#pragma once

// ─── CadenceController (Phase 3) ────────────────────────────────────────────
//
// Maximum-useful presentation cadence for a video scene:
//
//   presentationTarget = min(sourceFps × rate, displayHz, userCap)
//
// This is a PRESENTATION budget, never a decoder throttle: the decoder
// still decodes what the codec requires (paced to the source rate), and
// the scheduler selects useful frames. Above the target, decoded frames
// are skipped (drops); below it, the last frame re-presents. Playback
// stays 1x in both directions.
//
// Pure function so the table is unit-testable without a device.

namespace rex::wallpaper {

class CadenceController {
public:
    /// userCap <= 0 means uncapped (the caller maps config defaults).
    /// Degenerate source/display (<= 0) fall back to 60.
    static double PresentationTarget(double sourceFps, float rate,
                                     int displayHz, int userCap) {
        double demand = sourceFps > 0.0 ? sourceFps * rate : 60.0;
        double cap = displayHz > 0 ? static_cast<double>(displayHz) : 60.0;
        if (userCap > 0) {
            const double user = static_cast<double>(userCap);
            if (user < cap) {
                cap = user;
            }
        }
                double target = demand < cap ? demand : cap;
                return target > 0.0 ? target : 60.0;
    }

    // ─── Decode-ahead reserve (Phase D) ───────────────────────────────────
    //
    // The decoder paces itself so a small reserve of ALREADY-DECODED frames
    // survives a decoder stall. Normally it runs at the source rate; when the
    // reserve falls below the low-water mark it bursts (up to maxBurst) to
    // refill; once restored it returns to the source rate.
    //
    // This moves the GPU decode burst OFF the playback critical path instead of
    // trying to make the burst cheaper: the section-5 measurement showed
    // rendering/present continue on schedule through the stalls (23.7/24
    // presents, phWait 0.00 ms, GPU 111 ms), so front-loading the GPU work is
    // the correct lever.
    //
    // The reserve is also hard-capped by the shared 8-slot pool, so the
    // achievable duration depends on source rate - at 24 fps ~7 usable frames
    // is ~290 ms, at 120 fps it is only ~58 ms. Callers must report the
    // MEASURED reserve, never the nominal target.
    struct DecodeAheadPolicy {
        double lowWaterMs  = 100.0;
        double highWaterMs = 200.0;
        // Refill burst — default ON (2.0) since 2026-10-17.
        //
        // The 2026-10-15 A/B showed the refill eliminated underruns (Aari
        // AE_p95 34.04 -> 3.83 ms, under 3-8 -> 0; obanai under 24-30 -> 0)
        // but it was held off: with a reserve held, FrameScheduler::Tick's
        // catch-up fallback displayed frames whose PTS had not been reached,
        // so the displayed CONTENT ran ahead of the playhead (~2x) while
        // position and effFps still reported 1x. e5bfdf0 removed that
        // fallback (future frames are retained; starvation returns nullptr
        // and the render loop re-presents), which was the documented
        // precondition to re-enable — the hold-off below is resolved.
        // A/B re-run 2026-10-17 (harness, current build):
        //   Aari   24fps OFF res -6ms, jit 0.70ms, dwell max 16 vbl, under 0
        //                ON  res +122..163ms, jit 0.27-0.32, dwell max 6, under 0
        //   obanai 60fps OFF jit 5.09ms, under 11
        //                ON  jit 2.42ms, under 4 (clean windows 0.2-0.3ms)
        //   astral 60fps ON  res +57..62ms steady, jit 0.22-0.26, under 0
        // The content meter read 1.00x in every ON run (the exact 2x symptom
        // the hold-off existed to prevent). Rollback: REX_DECODE_AHEAD=1.
        double maxBurst    = 2.0;
        // Catch-up burst: applied ONLY when reserveMs < 0 (published content
        // is behind the wall clock). Refill burst (0..lowWater) stays on
        // maxBurst, so enabling catch-up cannot build the deep reserve that
        // triggered the 2x defect. Rationale (4K120 H.264, 2026-10-16): the
        // pacing sleep itself caps throughput at ~107 fps (8.33 ms sleep +
        // ~1 ms work < 119.9 needed), so the decoder can NEVER catch up at
        // burst 1.0 — reserve drains to seconds. 1.0 = off.
        // Default 2.0: engages ONLY while behind, where the alternative is
        // sustained failure; at/above water all pacing is unchanged (unit-
        // proven). Override with REX_CATCHUP_BURST (1 = force off).
        double catchUpBurst = 2.0;
    };

    /// The reserve is capped by the shared pool: ~7 usable frames, so the
    /// achievable duration SHRINKS as the source rate rises (291 ms at 24 fps,
    /// 117 ms at 60 fps, 58 ms at 120 fps). A fixed 100 ms low-water mark is
    /// therefore unreachable at >=60 fps, which made the decoder burst to the
    /// pool cap, park, and burst again every cycle - a measured throughput
    /// regression (schedDrops 204, effFps 52.5 on 60 fps content).
    ///
    /// Scale the marks to what is actually reachable, leaving headroom above
    /// the low-water so a refill does not immediately hit the pool cap.
    static constexpr int kUsableReserveSlots = 7;   // 8-slot pool, one in flight

    static DecodeAheadPolicy PolicyFor(double sourceFps,
                                      double slotCapSlots = kUsableReserveSlots) {
        DecodeAheadPolicy p;
        const double capMs = ReserveMs(static_cast<int>(slotCapSlots), sourceFps);
        if (capMs <= 0.0) return p;   // unknown rate: keep the nominal marks
        // Aim at 60% of the reachable ceiling, and never exceed the nominal
        // 100 ms low-water so low-rate content is unaffected.
        p.lowWaterMs  = std::min(100.0, capMs * 0.35);
        p.highWaterMs = std::min(200.0, capMs * 0.60);
        if (p.highWaterMs <= p.lowWaterMs) p.highWaterMs = p.lowWaterMs * 1.5;
        return p;
    }

    /// Interval between decodes, in milliseconds, given the current reserve
    /// depth (playhead-relative lead: published PTS minus wall target).
    /// Below the low-water mark the decoder bursts; in band it paces at the
    /// source rate; above the high-water mark it parks (decodes slower than
    /// realtime) so the playhead — not an ever-growing lead — bounds the
    /// ring. Behind the playhead (negative lead) it bursts to catch up and
    /// NEVER parks: parking a starved playhead is exactly the 0.6x
    /// regression of the reverted pool-full park. Pure so it is unit-testable
    /// without a device.
    ///
    /// P0-4: this function is STATELESS (a pure map from the current lead),
    /// recomputed every decode iteration. The reverted skip-pre-check
    /// deadlocked stop/restart because a latched "ring full, skip" survived
    /// the state change it was meant for; a stateless pace cannot stick.
    static constexpr double kParkFactor = 4.0;  // parked decode runs 4x slower
    static double DecodeIntervalMs(double sourceIntervalMs, double reserveMs,
                                   const DecodeAheadPolicy& p) {
        if (sourceIntervalMs <= 0.0) return 1.0;
        if (!(reserveMs >= 0.0) && !(reserveMs < 0.0)) return sourceIntervalMs;  // NaN
        const double burst = p.maxBurst > 1.0 ? p.maxBurst : 1.0;
        if (reserveMs < 0.0) {
            // Behind the playhead: catch up — EVERY negative lead bursts.
            // A 1x hold behind the playhead freezes the deficit (the decode
            // lag equals the playhead speed): an absorbing state. Measured
            // 2026-10-17 live: cold-load entries of -5..-40 ms stayed pinned
            // for minutes (late-frame storms, dwell smear 4-17 vblanks,
            // underruns); only an external pause/resume reset escaped. The
            // 2026-10-16 one-frame hysteresis treated shallow deficits as
            // pacing noise, but a properly refilled reserve cannot reach
            // negative from noise — only load/stall transients get here, and
            // those need the burst. Uses catchUpBurst, NOT maxBurst, so
            // rollback stays an explicit catchUpBurst = 1.0.
            const double cu = p.catchUpBurst > 1.0 ? p.catchUpBurst : 1.0;
            return sourceIntervalMs / cu;
        }
        if (reserveMs < p.lowWaterMs) {
            return sourceIntervalMs / burst;
        }
        if (reserveMs <= p.highWaterMs) return sourceIntervalMs;
        return sourceIntervalMs * kParkFactor;  // ahead: park, let the playhead catch up
    }

    /// Reserve depth in milliseconds for `frames` buffered source frames.
    static double ReserveMs(int frames, double sourceFps) {
        if (frames <= 0 || sourceFps <= 0.0) return 0.0;
        return 1000.0 * static_cast<double>(frames) / sourceFps;
    }
};

}  // namespace rex::wallpaper
