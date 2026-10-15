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
        double maxBurst    = 2.0;   // decode-ahead ceiling, 2x source rate
    };

    /// Interval between decodes, in ms, given the current reserve depth.
    /// Below the low-water mark the decoder bursts; otherwise it paces at the
    /// source rate. Pure so it is unit-testable without a device.
    static double DecodeIntervalMs(double sourceIntervalMs, double reserveMs,
                                   const DecodeAheadPolicy& p) {
        if (sourceIntervalMs <= 0.0) return 1.0;
        const double burst = p.maxBurst > 1.0 ? p.maxBurst : 1.0;
        if (reserveMs < p.lowWaterMs) {
            return sourceIntervalMs / burst;
        }
        return sourceIntervalMs;
    }

    /// Reserve depth in milliseconds for `frames` buffered source frames.
    static double ReserveMs(int frames, double sourceFps) {
        if (frames <= 0 || sourceFps <= 0.0) return 0.0;
        return 1000.0 * static_cast<double>(frames) / sourceFps;
    }
};

}  // namespace rex::wallpaper
