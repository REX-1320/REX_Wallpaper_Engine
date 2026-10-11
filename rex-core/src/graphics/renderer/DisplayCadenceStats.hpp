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

namespace rex::graphics {

class DisplayCadenceStats {
public:
    static constexpr size_t kRingSize = 256;

    // One sample per completed present.
    struct Sample {
        uint32_t presentRefreshCount = 0;  // vblanks this frame occupied
        int64_t  syncQpc = 0;             // 0 = not yet valid
        bool     valid = false;
    };

    /// Feed one sample. `syncQpc` is a monotonic QPC timestamp in whatever unit
    /// the caller uses; only differences between samples are meaningful, so the
    /// unit is irrelevant as long as it is consistent.
    void Add(const Sample& s) {
        if (!s.valid) return;
        const size_t idx = m_index % kRingSize;
        m_dwell[idx].store(s.presentRefreshCount, std::memory_order_relaxed);
        m_valid[idx].store(true, std::memory_order_relaxed);
        if (m_hasPrev.load(std::memory_order_relaxed) && s.syncQpc > m_prevQpc) {
            const auto delta = static_cast<double>(s.syncQpc - m_prevQpc);
            m_gapMicros[idx].store(delta, std::memory_order_relaxed);
            m_hasGap.store(true, std::memory_order_relaxed);
        }
        m_prevQpc.store(s.syncQpc, std::memory_order_relaxed);
        m_hasPrev.store(true, std::memory_order_relaxed);
        m_index = idx + 1;
        m_count.store(m_count.load(std::memory_order_relaxed) + 1,
                      std::memory_order_relaxed);
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
    std::atomic<size_t> m_index{0};
    std::atomic<size_t> m_count{0};
    std::atomic<bool> m_hasPrev{false};
    std::atomic<bool> m_hasGap{false};
};

}  // namespace rex::graphics