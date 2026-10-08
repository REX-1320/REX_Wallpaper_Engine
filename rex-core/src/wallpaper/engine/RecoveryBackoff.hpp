#pragma once

namespace rex::wallpaper {

// ─── Recovery retry policy ─────────────────────────────────────────────────
//
// The render loop retries device-loss / desktop-host recovery whenever the
// renderer reports removal or the host asks for a rebuild. A retry delay is
// mandatory, not cosmetic: recovery cannot succeed while the shell is still
// restarting (Explorer gone → no WorkerW to acquire), so retrying on the very
// next iteration pinned the render thread at 100% CPU — and, because the
// frame counter stopped advancing, ran the %30/%120 housekeeping on every pass
// (GetForegroundWindow + EnumDisplayMonitors per iteration).
//
// First retry is quick because an Explorer restart typically completes within
// a second or two; the delay then doubles up to the cap so a host that never
// comes back costs one attempt per interval instead of a busy loop.

/// Upper bound for the recovery retry delay.
inline constexpr int kMaxRecoveryDelayMs = 5000;

/// Delay to wait before the next recovery attempt, given the delay used for
/// the previous one. Always strictly positive — a zero delay is the hot-spin.
inline int NextRecoveryDelayMs(int currentDelayMs) {
    constexpr int kFirstDelayMs = 250;
    if (currentDelayMs <= 0) return kFirstDelayMs;
    if (currentDelayMs >= kMaxRecoveryDelayMs) return kMaxRecoveryDelayMs;
    const int doubled = currentDelayMs * 2;
    return doubled < kMaxRecoveryDelayMs ? doubled : kMaxRecoveryDelayMs;
}

} // namespace rex::wallpaper
