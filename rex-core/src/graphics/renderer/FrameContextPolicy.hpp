#pragma once

// ─── Frame-context reset gating (P0-4 / R17) ────────────────────────────────
//
// A command allocator may only be Reset() after the GPU finished executing
// every list recorded into it. The renderer keeps a ring of FrameContexts
// (one allocator + the fence value signaled after its work); BeginFrame
// waits for the selected context's fence before resetting it.

#include <cstdint>

namespace rex::graphics {

/// Number of in-flight frame contexts (matches the swapchain buffer count:
/// with max frame latency 2 the GPU is at most 2 frames behind, so a
/// 3-deep ring always has a completed context absent a real hitch).
inline constexpr unsigned kFrameContextCount = 3;

/// Ring slot for the given frame number.
inline unsigned FrameContextIndex(uint64_t frameNumber, unsigned count) {
    return static_cast<unsigned>(frameNumber % count);
}

/// True when the context may be reset: its fence completed (or it was never
/// used — fence 0 against a queue starting at 0).
inline bool FrameContextReady(uint64_t contextFenceValue,
                              uint64_t completedFenceValue) {
    return contextFenceValue <= completedFenceValue;
}

}  // namespace rex::graphics
