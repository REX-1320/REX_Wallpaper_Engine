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
};

}  // namespace rex::wallpaper
