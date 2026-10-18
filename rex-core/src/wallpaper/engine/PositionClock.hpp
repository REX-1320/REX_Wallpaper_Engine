#pragma once

// ─── PositionClock ──────────────────────────────────────────────────────
//
// Cross-thread playback-position clock for Engine (audit 2026-10-17, #3).
// The render thread reads it on every pass (Update, auto-pause / occlusion
// arbitration) while the command thread drives the transitions (Play /
// Pause / Stop / LoadScene / device-loss recovery). A plain
// shared::Stopwatch is not thread-safe, and the render thread can never
// take Engine::m_mutex (Shutdown holds it across the render-thread join),
// so the clock state is published through a seqlock: transition writers
// serialize on a small spinlock (short scalar math only — never joins or
// blocking calls) and bump the sequence around their stores; readers take
// a consistent {base, start, running} snapshot with bounded retries.
// Readers never block; if a writer is preempted mid-update the reader
// falls back to the base alone — a one-transition-stale position readout,
// never a torn pair.
//
// Semantics mirror the old Stopwatch + base folding:
//   position(t) = base + (running ? (t - start) * rate : 0)
// Start() re-stamps from zero (elapsed resets), so pause/park transitions
// must Fold() the elapsed into the base first — otherwise every resume
// rewinded the reported position while the decoder kept its own PTS.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace rex::wallpaper {

class PositionClock {
public:
    /// Monotonic wall-clock now in seconds (steady_clock: immune to system
    /// clock adjustments).
    static double Now() {
        return std::chrono::duration<double>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    /// Begin counting from `nowS`; the base is untouched.
    void Start(double nowS) {
        Guard lock(*this);
        Store(m_base.load(std::memory_order_relaxed), nowS, true);
    }

    /// Stop counting; the base remains (position freezes at the base).
    void Reset() {
        Guard lock(*this);
        Store(m_base.load(std::memory_order_relaxed),
              m_start.load(std::memory_order_relaxed), false);
    }

    /// Overwrite the base (seconds) — used on load / seek / recovery.
    void SetBase(double baseSeconds) {
        Guard lock(*this);
        Store(baseSeconds, m_start.load(std::memory_order_relaxed),
              m_running.load(std::memory_order_relaxed));
    }

    /// Fold the elapsed (rate-scaled) time into the base and stop.
    /// Returns the new base. Concurrent folds serialize on the transition
    /// lock: the loser observes running == false and adds nothing, so a
    /// user Pause racing an occlusion park banks the lapse once.
    double Fold(double nowS, double rate) {
        Guard lock(*this);
        double base = m_base.load(std::memory_order_relaxed);
        if (m_running.load(std::memory_order_relaxed)) {
            base += (nowS - m_start.load(std::memory_order_relaxed)) * rate;
        }
        Store(base, nowS, false);
        return base;
    }

    /// Position at `nowS`: base + elapsed * rate while running, else base.
    double Position(double nowS, double rate = 1.0) const {
        const Snapshot s = Load();
        return s.base + (s.running ? (nowS - s.start) * rate : 0.0);
    }

    bool Running() const { return Load().running; }

private:
    struct Snapshot {
        double base = 0.0;
        double start = 0.0;
        bool running = false;
    };

    // Seqlock: odd sequence = writer mid-update, even = stable.
    void Store(double base, double start, bool running) {
        m_seq.fetch_add(1, std::memory_order_acq_rel);
        m_base.store(base, std::memory_order_relaxed);
        m_start.store(start, std::memory_order_relaxed);
        m_running.store(running, std::memory_order_relaxed);
        m_seq.fetch_add(1, std::memory_order_acq_rel);
    }

    Snapshot Load() const {
        for (int attempt = 0; attempt < 64; ++attempt) {
            const uint32_t s1 = m_seq.load(std::memory_order_acquire);
            if (s1 & 1u) continue;  // writer mid-update
            const Snapshot snap{m_base.load(std::memory_order_relaxed),
                                m_start.load(std::memory_order_relaxed),
                                m_running.load(std::memory_order_relaxed)};
            std::atomic_thread_fence(std::memory_order_acquire);
            if (m_seq.load(std::memory_order_acquire) == s1) return snap;
        }
        // Writer preempted mid-update (rare): conservative best-effort —
        // base alone, never extrapolate from a possibly-mixed pair.
        return Snapshot{m_base.load(std::memory_order_relaxed), 0.0, false};
    }

    // Transition spinlock. Writers hold it for a few scalar stores only;
    // readers never take it (see Load()).
    class Guard {
    public:
        explicit Guard(const PositionClock& clock) : m_clock(clock) {
            while (m_clock.m_lock.test_and_set(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        }
        ~Guard() { m_clock.m_lock.clear(std::memory_order_release); }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;

    private:
        const PositionClock& m_clock;
    };

    mutable std::atomic_flag m_lock{};  // C++20: value-init = clear
    mutable std::atomic<uint32_t> m_seq{0};
    std::atomic<double> m_base{0.0};
    std::atomic<double> m_start{0.0};
    std::atomic<bool> m_running{false};
};

}  // namespace rex::wallpaper
