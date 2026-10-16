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
        // Controlled A/B (2026-10-15, 4 runs/arm, drops+repeats as deltas over
        // a 15 s window, system load held under 20%):
        //   Aari  24fps  OFF AE_p95 34.04 / drops 0.1/s / effFps 23.4 / under 3-8
        //                  ON  AE_p95  3.83 / drops 0.0/s / effFps 24.0 / under 0
        //   obanai 60fps OFF AE_p95  2.74 / drops 0.1/s / effFps 59.1 / under 24-30
        //                  ON  AE_p95  0.61 / drops 0.9/s / effFps 60.0 / under 0
        // Timing gains are real and are NOT bought with dropped frames on Aari.
        //
        // HELD OFF DESPITE THAT, because the reserve triggers a USER-VISIBLE
        // defect the timing metrics cannot see: with future frames buffered,
        // FrameScheduler::Tick selects the NEWEST due frame (FrameScheduler.cpp
        // :51) and its catch-up fallback returns the newest published frame
        // regardless of PTS (:64-77), so displayed CONTENT advances in jumps
        // while the playhead advances 1x — the wallpaper reads as playing at
        // ~2x. Position still reports 1.000x and effFps still reports the
        // source rate, so no existing telemetry detects it.
        //
        // Correct fix: make the scheduler consume the frame closest to the
        // playhead (oldest due) and stop the catch-up fallback from firing
        // merely because the ring holds future frames. Do not re-enable the
        // burst before that lands.
        double maxBurst    = 1.0;   // 1.0 = burst held off (see above)
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
        if (reserveMs < 0.0) return sourceIntervalMs / burst;  // behind: catch up
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
