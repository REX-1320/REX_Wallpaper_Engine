#pragma once

// ─── Occlusion-park decision (vendor-neutral idle optimization) ─────────────
//
// When every present path reports DXGI_STATUS_OCCLUDED, DWM is dropping our
// frames because the wallpaper is fully covered (maximized window, lock
// screen, Task View) — yet the engine keeps decoding + drawing + presenting
// at full rate into the void. This header turns that signal into a park
// decision with three pieces:
//
//   OcclusionTracker — renderer seam. Folds raw present HRESULTs into a
//     sticky per-path occluded flag. ONLY the documented DXGI success code
//     counts: real failures (E_FAIL, DEVICE_REMOVED, ...) clear the flag
//     because the device path — not DWM culling — owns what happens next.
//   AllPathsOccluded — aggregates the main renderer + per-monitor
//     compositor states. Partial occlusion (any path visible) never parks
//     decode; per-path Draw/Present skipping is the Engine's job.
//   OcclusionPolicy — consecutive-occlusion time → Park exactly once;
//     any visible sample resets the timer, or Resumes exactly once when
//     parked. Starts DISARMED (fresh-LOAD grace): only time accumulated
//     after Arm() counts, so spurious early OCCLUDEDs can never park.
//
// Vendor neutrality is by construction, not by testing: the ONLY driver
// observable matched here is one documented DXGI HRESULT, compared by
// value — no dxgi.h, no NVAPI, no AMD/Intel extensions, no adapter-LUID
// branches. AMD / NVIDIA / Intel behave identically under this contract:
// a driver that never reports OCCLUDED simply never parks (today's
// behavior, byte for byte). Park/resume transitions are loud in the log
// and visible in GET_STATUS so any future vendor quirk is diagnosable.
//
// Pure + header-only so the whole decision is unit-testable without an
// engine or a GPU (mirrors PlaybackPolicy.hpp). Threading: Arm/Reset come
// from the command thread (LOAD/Play/Pause/Stop/Unload), Update/IsParked
// from the render thread — atomics throughout, never blocks.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace rex::wallpaper {

enum class OcclusionAction { None, Park, Resume };

class OcclusionTracker {
public:
    // Documented DXGI_STATUS_OCCLUDED (0x087A0001, a SUCCESS code).
    // Matched by value so this layer needs no DXGI headers.
    static constexpr long kPresentOccluded = 0x087A0001L;

    void NotifyPresent(long presentHr) {
        if (presentHr == kPresentOccluded) {
            ++consecutiveOccluded_;
            occluded_ = true;
        } else {
            // S_OK clears the run; real failures end it too — DWM is no
            // longer dropping frames, the device path is. Recovery (not
            // parking) owns failures.
            consecutiveOccluded_ = 0;
            occluded_ = false;
        }
    }

    bool IsOccluded() const { return occluded_; }
    uint32_t ConsecutiveOccluded() const { return consecutiveOccluded_; }

private:
    bool occluded_ = false;
    uint32_t consecutiveOccluded_ = 0;
};

inline bool AllPathsOccluded(bool mainOccluded,
                             const std::vector<bool>& compositorOccluded) {
    if (!mainOccluded) return false;
    for (bool occluded : compositorOccluded) {
        if (!occluded) return false;
    }
    return true;
}

class OcclusionPolicy {
public:
    explicit OcclusionPolicy(double parkAfterSeconds = 1.0)
        : parkAfterSeconds_(parkAfterSeconds) {}

    // LOAD/PLAY arms the clock; Reset (STOP/UNLOAD/seek) disarms it.
    // Arm/Reset come from the command thread, Update/IsParked from the
    // render thread — atomics throughout, never blocks.
    void Arm() { armed_.store(true, std::memory_order_release); }
    void Reset() {
        armed_.store(false, std::memory_order_release);
        occludedTime_.store(0.0, std::memory_order_relaxed);
        parked_.store(false, std::memory_order_release);
    }

    OcclusionAction Update(bool allOccluded, double dtSeconds) {
        if (!armed_.load(std::memory_order_acquire)) return OcclusionAction::None;
        if (parked_.load(std::memory_order_acquire)) {
            if (!allOccluded) {
                parked_.store(false, std::memory_order_release);
                occludedTime_.store(0.0, std::memory_order_relaxed);
                return OcclusionAction::Resume;
            }
            return OcclusionAction::None;
        }
        if (!allOccluded) {
            occludedTime_.store(0.0, std::memory_order_relaxed);
            return OcclusionAction::None;
        }
        if (dtSeconds > 0.0) {
            occludedTime_.store(occludedTime_.load(std::memory_order_relaxed) + dtSeconds,
                                std::memory_order_relaxed);
        }
        // Epsilon: dt accumulates binary error (e.g. 60 x 1/60 lands a hair
        // under 1.0); the threshold is a time, not an exact sum.
        if (occludedTime_.load(std::memory_order_relaxed) + 1e-9 >= parkAfterSeconds_) {
            parked_.store(true, std::memory_order_release);
            return OcclusionAction::Park;
        }
        return OcclusionAction::None;
    }

    bool IsParked() const { return parked_.load(std::memory_order_acquire); }

private:
    double parkAfterSeconds_ = 1.0;
    std::atomic<bool> armed_{false};
    std::atomic<bool> parked_{false};
    std::atomic<double> occludedTime_{0.0};
};

}  // namespace rex::wallpaper
