#pragma once

// ─── Render cadence policy (P2-15 / R18) ────────────────────────────────────
//
// The render loop must wake at the SLOWER of the configured maxFps and the
// decoder's measured cadence — waking at full rate against a 30 fps decode
// burns wakeups for identical frames. maxFps itself is clamped (0 = default
// 60; absurd values capped at 240) since the IPC value was unbounded.

#include <algorithm>

namespace rex::wallpaper {

/// Render-loop interval in milliseconds.
inline double RenderIntervalMs(int maxFps, double decoderFps) {
    double fps = maxFps <= 0 ? 60.0 : std::min<double>(maxFps, 240);
    if (decoderFps > 0.0) {
        fps = std::min(fps, decoderFps);
    }
    return std::max(1000.0 / fps, 4.0);
}

}  // namespace rex::wallpaper
