#pragma once

#include "wallpaper/decoder/FfmpegVideoDecoder.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace rex::wallpaper {

// ─── FrameScheduler ─────────────────────────────────────────────────────
//
// SPSC ring of GPU-backed VideoFrames. Exactly one producer (decode thread)
// calls Push(); exactly one consumer (render thread) calls Tick().
//
// ─── Semantics ──────────────────────────────────────────────────────────
//
// PTS-due selection (Phase 2: clock separation): the consumer asks "which
// decoded frame is due at wall time T" and receives the newest published
// frame with pts <= T (+ tolerance). Bypassed frames and frames overwritten
// unseen count as dropped (GetDroppedCount); equal-PTS repeats are free.
// A decoder shortfall therefore costs frames, never time — playback stays
// at 1x while unique-frame rate follows decode throughput.
//
// Frame slots are a stable ring array and the underlying pool textures are
// owned by the decoder for the process lifetime, so the returned pointer
// stays valid and no handles are ever closed here. GPU content safety is
// guaranteed by the D3D11->D3D12 fence: the consumer must wait on
// GetSlotFence(frame.slot) before sampling (see FfmpegVideoDecoder).
//
// ─── SPSC guarantee ─────────────────────────────────────────────────────
//
// Producer writes slot (wc % N), consumer reads slot ((wc - 1) % N); with
// the single-producer/single-consumer invariant these are always distinct
// indices, so no mutex is needed — only acquire/release atomics on the
// write count. If multiple producers or consumers are ever added, this
// class MUST be redesigned.

class FrameScheduler {
public:
    static constexpr size_t kNumSlots = 8;
    /// Selection tolerance: a frame up to this far past the wall target
    /// still counts as due (prevents systematic one-frame lateness on
    /// fractional rates like 59.94).
    static constexpr double kSelectToleranceSec = 0.008;

    /// Producer: publish a frame. Overwrites the oldest unpublished slot;
    /// that slot is only read by the consumer after it has already moved
    /// past it, so no torn reads are possible.
    void Push(const VideoFrame& frame);

    /// Consumer: returns the newest published frame due at targetPts
    /// (pts <= targetPts + tolerance), or nullptr when nothing is due.
    /// Skipped and overwritten-unseen frames accumulate in the drop
    /// counter. Never blocks.
    const VideoFrame* Tick(double targetPts);

    /// Cumulative dropped frames (bypassed by selection + overwritten
    /// unseen; equal-PTS repeats excluded). Written by the consumer in
    /// Tick; safe to read from any thread (telemetry).
    uint64_t GetDroppedCount() const {
        return m_dropped.load(std::memory_order_acquire);
    }

    /// Drop any unpublished frame (P1-5 / R10). Called on seek, loop wrap
    /// and recovery resume so a pre-seek frame is never presented: after
    /// Clear, Tick returns nullptr until the next Push. Safe from any
    /// thread (Seek/command thread, decode thread); the consumer stays
    /// lock-free.
    void Clear();

    size_t GetNumSlots() const { return kNumSlots; }

    /// Published-but-unconsumed frame count now (Phase 1 queue depth).
    /// Clamped to the ring size: anything older was overwritten unseen.
    size_t PendingCount() const {
        const uint64_t wc = m_writeCount.load(std::memory_order_acquire);
        const uint64_t lc = m_lastConsumedWrite.load(std::memory_order_acquire);
        if (wc <= lc) return 0;
        const uint64_t pending = wc - lc;
        return static_cast<size_t>(pending > kNumSlots ? kNumSlots : pending);
    }

private:
    std::array<VideoFrame, kNumSlots> m_slots{};
    std::atomic<uint64_t> m_writeCount{0};
    // Last consumed write. Written by the consumer in Tick AND by Clear
    // from other threads — hence atomic (was consumer-local plain uint64).
    std::atomic<uint64_t> m_lastConsumedWrite{0};
    // Cumulative drops, consumer-written (see Tick), read by telemetry.
    std::atomic<uint64_t> m_dropped{0};
    // High-water mark for vanished-write accounting (see Tick): each
    // overwritten-unseen write is counted at most once, no matter how
    // many starved Ticks observe the same stale gap. Reset by Clear
    // alongside the consumed write.
    std::atomic<uint64_t> m_countedThrough{0};
};

} // namespace rex::wallpaper
