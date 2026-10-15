#pragma once

// ─── Display-cadence accumulator ───────────────────────────────────────────
//
// Answers the question REX's own telemetry structurally cannot: not "when did
// we submit", but "when did the frame actually reach the panel, and for how many
// display refreshes".
//
// Source is DXGI (IDXGISwapChain::GetFrameStatistics, dxgi.h):
//   PresentRefreshCount — refreshes the frame was displayed for
//   PresentCount        — cumulative presents
//   SyncRefreshCount    — cumulative synced presents
//   SyncQPCTime         — QPC timestamp of the last sync
//
// This header is deliberately free of DXGI types: it is a pure accumulator so
// the statistics can be unit-tested without a swap chain, a GPU, or a display.
// The adapter (SwapChain) supplies raw samples.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

// SyncQPCTime is reported in RAW QueryPerformanceCounter ticks, while the
// render loop's clock (high_resolution_clock == steady_clock on MSVC) reports
// NANOSECONDS. Absolute comparison needs an explicit conversion, and the
// frequency must be queried rather than assumed 10 MHz — MSVC notes 10 MHz is
// merely "very common on modern x86/x64" with a 24 MHz case.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace rex::graphics {

class DisplayCadenceStats {
public:
    static constexpr size_t kRingSize = 256;
    /// Minimum SyncRefreshCount advance before a sample contributes to the
    /// period estimate. A single-vblank delta amplifies QPC sampling jitter by
    /// the full period, which would poison the estimate.
    static constexpr int64_t kMinRefreshDelta = 2;

    /// QueryPerformanceFrequency cached once. Returns ticks-per-second.
    static double QpcHz() {
        static const double hz = [] {
            LARGE_INTEGER f{};
            if (!QueryPerformanceFrequency(&f) || f.QuadPart <= 0) return 0.0;
            return static_cast<double>(f.QuadPart);
        }();
        return hz;
    }

    // One sample per completed present.
    struct Sample {
        uint32_t presentRefreshCount = 0;  // vblanks this frame occupied
        int64_t  syncQpc = 0;             // 0 = not yet valid
        // DXGI documents (SyncRefreshCount, SyncQPCTime) as a PAIR: count k
        // corresponds to time T. That makes the pair a self-consistent anchor
        // on the vblank lattice — t(j) = T + (j - k) * P — which stays valid
        // even when read late, because it names a lattice point rather than
        // "now". This is the only non-DWM phase reference available to a
        // WorkerW surface that presents as an independent-flip overlay.
        uint32_t syncRefreshCount = 0;
        bool     valid = false;
    };

    /// Feed one sample. `syncQpc` is a monotonic QPC timestamp in whatever unit
    /// the caller uses; only differences between samples are meaningful, so the
    /// unit is irrelevant as long as it is consistent.
    void Add(const Sample& s) {
        if (!s.valid) return;
        const size_t idx = m_index % kRingSize;
        m_dwell[idx].store(s.presentRefreshCount, std::memory_order_relaxed);
        // Track the lattice anchor every valid sample, independent of whether
        // a gap could be computed.
        m_lastSyncRefresh.store(s.syncRefreshCount, std::memory_order_relaxed);
        m_lastSyncQpc.store(s.syncQpc, std::memory_order_relaxed);
        const bool havePrev = m_hasPrev.load(std::memory_order_relaxed);
        const bool forward = s.syncQpc > m_prevQpc.load(std::memory_order_relaxed);
        if (havePrev && forward) {
            const auto deltaQpc = s.syncQpc - m_prevQpc.load(std::memory_order_relaxed);
            const int64_t deltaK = static_cast<int64_t>(s.syncRefreshCount) -
                                   static_cast<int64_t>(m_prevRefresh.load(std::memory_order_relaxed));
            m_gapMicros[idx].store(static_cast<double>(deltaQpc), std::memory_order_relaxed);
            m_valid[idx].store(true, std::memory_order_relaxed);
            m_hasGap.store(true, std::memory_order_relaxed);
            // Refresh-period estimate: only meaningful when the vblank counter
            // advanced by a useful amount (a single vblank amplifies jitter).
            if (deltaK >= kMinRefreshDelta && deltaQpc > 0) {
                // Accumulate raw ticks and counts separately, then divide ONCE.
                // Averaging per-sample ratios weights a 4-vblank sample the same
                // as a 400-vblank one, which skews the estimate badly when the
                // delta size varies (it does, at 60 fps on a 160 Hz panel).
                m_periodQpcSum.store(
                    m_periodQpcSum.load(std::memory_order_relaxed) + deltaQpc,
                    std::memory_order_relaxed);
                m_periodKSum.store(
                    m_periodKSum.load(std::memory_order_relaxed) + deltaK,
                    std::memory_order_relaxed);
                m_periodCount.store(m_periodCount.load(std::memory_order_relaxed) + 1,
                                    std::memory_order_relaxed);
            }
        } else {
            m_valid[idx].store(false, std::memory_order_relaxed);
            // Only advance the baseline on a FORWARD sample. Advancing it
            // unconditionally on a backwards one poisons the next gap by
            // spanning the whole excursion.
            if (forward || !havePrev) {
                m_prevQpc.store(s.syncQpc, std::memory_order_relaxed);
                m_prevRefresh.store(s.syncRefreshCount, std::memory_order_relaxed);
                m_hasPrev.store(true, std::memory_order_relaxed);
            } else {
                // Discontinuous (mode change): re-baseline.
                m_prevQpc.store(s.syncQpc, std::memory_order_relaxed);
                m_prevRefresh.store(s.syncRefreshCount, std::memory_order_relaxed);
                m_hasPrev.store(true, std::memory_order_relaxed);
            }
        }
        if (!havePrev) {
            m_prevQpc.store(s.syncQpc, std::memory_order_relaxed);
            m_prevRefresh.store(s.syncRefreshCount, std::memory_order_relaxed);
            m_hasPrev.store(true, std::memory_order_relaxed);
        }
        m_index = idx + 1;
        m_count.store(m_count.load(std::memory_order_relaxed) + 1,
                      std::memory_order_relaxed);
    }

    /// Estimated refresh period in microseconds, measured as
    /// (sum dSyncQPCTime / sum dSyncRefreshCount) converted from QPC ticks.
    /// Returns 0 until enough forward samples with a useful counter delta exist.
    double EstimatedPeriodMicros() const {
        const int64_t kSum = m_periodKSum.load(std::memory_order_relaxed);
        if (kSum <= 0) return 0.0;
        const double hz = QpcHz();
        if (hz <= 0.0) return 0.0;
        const double ticksPerVblank = static_cast<double>(
            m_periodQpcSum.load(std::memory_order_relaxed)) /
            static_cast<double>(kSum);
        return ticksPerVblank * 1e6 / hz;
    }

    /// Last lattice anchor (SyncRefreshCount, SyncQPCTime in QPC ticks).
    uint32_t LastSyncRefreshCount() const {
        return m_lastSyncRefresh.load(std::memory_order_relaxed);
    }
    int64_t LastSyncQpc() const {
        return m_lastSyncQpc.load(std::memory_order_relaxed);
    }
    uint64_t PeriodSamples() const {
        return m_periodCount.load(std::memory_order_relaxed);
    }

    /// Percentile over collected values. p in [0,1]. Returns 0 when empty.
    double PercentileDwell(double p) const { return Percentile(m_dwell, p); }
    double PercentileGapMicros(double p) const { return Percentile(m_gapMicros, p); }
    double MaxGapMicros() const { return Percentile(m_gapMicros, 1.0); }
    size_t Samples() const { return m_count.load(std::memory_order_relaxed); }

    /// True once enough samples exist to judge cadence.
    bool HasCadence() const {
        return m_count.load(std::memory_order_relaxed) >= kRingSize;
    }

    void Reset() {
        for (size_t i = 0; i < kRingSize; ++i) {
            m_valid[i].store(false, std::memory_order_relaxed);
        }
        m_hasPrev.store(false, std::memory_order_relaxed);
        m_hasGap.store(false, std::memory_order_relaxed);
        m_periodQpcSum.store(0, std::memory_order_relaxed);
        m_periodKSum.store(0, std::memory_order_relaxed);
        m_periodCount.store(0, std::memory_order_relaxed);
        m_lastSyncRefresh.store(0, std::memory_order_relaxed);
        m_lastSyncQpc.store(0, std::memory_order_relaxed);
        m_count.store(0, std::memory_order_relaxed);
        m_index = 0;
    }

private:
    using Bucket = std::atomic<double>;

    double Percentile(const std::array<Bucket, kRingSize>& ring, double p) const {
        std::array<double, kRingSize> tmp{};
        size_t n = 0;
        for (size_t i = 0; i < kRingSize; ++i) {
            if (!m_valid[i].load(std::memory_order_relaxed)) continue;
            tmp[n++] = ring[i].load(std::memory_order_relaxed);
        }
        if (n == 0) return 0.0;
        std::sort(tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(n));
        const size_t idx = static_cast<size_t>(p * static_cast<double>(n - 1));
        return tmp[idx];
    }

    std::array<Bucket, kRingSize> m_dwell{};
    std::array<Bucket, kRingSize> m_gapMicros{};
    std::array<std::atomic<bool>, kRingSize> m_valid{};
    std::atomic<int64_t> m_prevQpc{0};
    std::atomic<uint32_t> m_prevRefresh{0};
    std::atomic<uint32_t> m_lastSyncRefresh{0};
    std::atomic<int64_t> m_lastSyncQpc{0};
    std::atomic<int64_t> m_periodQpcSum{0};
    std::atomic<int64_t> m_periodKSum{0};
    std::atomic<uint64_t> m_periodCount{0};
    std::atomic<size_t> m_index{0};
    std::atomic<size_t> m_count{0};
    std::atomic<bool> m_hasPrev{false};
    std::atomic<bool> m_hasGap{false};
};

}  // namespace rex::graphics