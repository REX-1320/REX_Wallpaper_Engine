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
// Latest-frame-wins: the consumer always receives the most recently pushed
// frame; intermediate frames are counted as dropped. This is the correct
// policy for wallpaper playback (never queue more than one frame of latency).
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

    /// Producer: publish a frame. Overwrites the oldest unpublished slot;
    /// that slot is only read by the consumer after it has already moved
    /// past it, so no torn reads are possible.
    void Push(const VideoFrame& frame);

    /// Consumer: returns the latest published frame since the last call,
    /// or nullptr if nothing new arrived. Never blocks.
    const VideoFrame* Tick(double targetPts);

    size_t GetNumSlots() const { return kNumSlots; }

private:
    std::array<VideoFrame, kNumSlots> m_slots{};
    std::atomic<uint64_t> m_writeCount{0};
    uint64_t m_lastConsumedWrite = 0;  // consumer-local
};

} // namespace rex::wallpaper
