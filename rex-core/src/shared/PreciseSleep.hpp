#pragma once

// ─── PreciseSleepUntil ────────────────────────────────────────────────────
//
// High-resolution sleep until an absolute steady_clock deadline, precise to
// ~0.5 ms. Backed by a CREATE_WAITABLE_TIMER_HIGH_RESOLUTION waitable timer
// (Windows 10 1809+), which is accurate regardless of the process timer
// quantum.
//
// Why this exists: since Windows 11 24H2 / 25H2 (build 26100+) the OS no
// longer honors timeBeginPeriod(1) — the system timer stays at the default
// 15.6 ms quantum, so Sleep()/sleep_until() round up to the next quantum
// (~31 ms for a 16.7 ms sleep → 32 fps instead of 60). The high-resolution
// waitable timer is the supported way to get fine-grained waits on those
// builds.
//
// Falls back to sleep_until() + a spin of the tail for very short sleeps
// (or when the flag is unsupported).

#include <chrono>
#include <thread>

#include <windows.h>

namespace rex::shared {

template <typename Clock, typename Duration>
inline void PreciseSleepUntil(std::chrono::time_point<Clock, Duration> deadline) {
    static_assert(std::is_same<Clock, std::chrono::steady_clock>::value,
                  "PreciseSleepUntil requires a steady_clock deadline");
    using namespace std::chrono;
    const auto now = steady_clock::now();
    if (deadline <= now) {
        return;
    }

    const auto remainingUs =
        duration_cast<microseconds>(deadline - now).count();

    // Only bother with the waitable timer for meaningful sleeps; spin the
    // tail for anything shorter than ~2 ms (SetWaitableTimer has fixed
    // overhead too).
    if (remainingUs > 2000) {
        static thread_local HANDLE s_timer = [] {
            return CreateWaitableTimerExW(
                nullptr, nullptr,
                CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        }();
        if (s_timer) {
            LARGE_INTEGER due;
            due.QuadPart = -10 * remainingUs;  // relative, 100 ns units
            if (SetWaitableTimer(s_timer, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(s_timer, INFINITE);
                return;
            }
        }
    }

    // Fallback: coarse sleep_until (quantum-limited wake), then spin the
    // last ~1.5 ms so the deadline is still honored.
    const auto coarse = deadline - microseconds(1500);
    if (coarse > now) {
        std::this_thread::sleep_until(coarse);
    }
    while (steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
}

} // namespace rex::shared
