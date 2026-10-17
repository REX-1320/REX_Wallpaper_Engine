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

/// One render-pass decision for device-loss / desktop-host recovery.
struct RecoveryDecision {
    /// Run the recovery procedure this pass.
    bool attempt = false;
    /// Clear the host request flag. True only when an attempt runs with a
    /// host request outstanding.
    bool consumeRequest = false;
};

/// Pure gate for the render loop's recovery block. An attempt may run only
/// after the backoff deadline (`readyToAttempt`). The host request flag must
/// be consumed AT ATTEMPT TIME: the pre-fix loop read it with
/// exchange(false) before the deadline check, so a failed attempt's re-armed
/// request was cleared on the next in-window pass and the retry never ran —
/// host-only recovery (Explorer restart, no device loss) bricked the
/// wallpaper until app restart.
inline RecoveryDecision DecideRecovery(bool hostRequested, bool deviceLost,
                                       bool readyToAttempt) {
    RecoveryDecision d;
    if (!hostRequested && !deviceLost) return d;
    if (!readyToAttempt) return d;  // retain the request across the window
    d.attempt = true;
    d.consumeRequest = hostRequested;
    return d;
}

} // namespace rex::wallpaper
