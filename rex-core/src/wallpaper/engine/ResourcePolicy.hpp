#pragma once

#include <cstdint>
#include <string>

// ─── Media resource envelope (P1) ───────────────────────────────────────────
//
// Decide BEFORE presenting whether a video's GPU footprint fits, so
// oversized media fails LOAD loudly (ok:false) instead of silently
// exhausting the pool/adapter mid-playback. Pure + header-only so the rule
// is unit-testable without an engine (mirrors PowerPolicy.hpp).
//
// Footprint convention: w*h*4 bytes per slot (conservative RGBA sizing,
// same as the ringBytes telemetry) across BOTH resident rings — the decode
// pool and the scheduler ring. Slot counts mirror
// FfmpegVideoDecoder::kPoolSize and FrameScheduler::kNumSlots (pinned equal
// by MediaBudgetMirrorTest — update together).
//
// Unknown budget (0: query chain broken) FAILS OPEN with a warning: refusing
// valid wallpapers on unqueryable adapters (remote sessions, old drivers)
// is worse than the risk it guards. No arbitrary resolution cap: 8K media
// on a big budget loads fine; absurd dimensions (>16384/axis, i.e. corrupt
// headers, not cameras) always refuse.

namespace rex::wallpaper {

inline constexpr uint64_t kDecodePoolSlots = 8;
inline constexpr uint64_t kSchedulerRingSlots = 8;
inline constexpr int kMaxDimension = 16384;
inline constexpr uint64_t kPoolBytesPerPixel = 4;

struct MediaBudget {
    uint64_t poolBytes = 0;
    bool overBudget = false;
    bool unknownBudget = false;  // budget==0: allowed, warn
    std::string reason;
};

inline uint64_t PoolFootprintBytes(int width, int height) {
    return static_cast<uint64_t>(width) * static_cast<uint64_t>(height) *
           kPoolBytesPerPixel * (kDecodePoolSlots + kSchedulerRingSlots);
}

inline MediaBudget AssessMediaBudget(int width, int height,
                                     uint64_t budgetBytes) {
    MediaBudget r;
    if (width <= 0 || height <= 0) {
        r.overBudget = true;
        r.reason = "invalid video dimensions";
        return r;
    }
    if (width > kMaxDimension || height > kMaxDimension) {
        r.overBudget = true;
        r.reason = "video dimensions exceed sanity cap";
        return r;
    }
    r.poolBytes = PoolFootprintBytes(width, height);
    if (budgetBytes == 0) {
        r.unknownBudget = true;
        r.reason = "GPU budget unqueryable; allowing with warning";
        return r;
    }
    if (r.poolBytes > budgetBytes) {
        r.overBudget = true;
        r.reason = "estimated pool footprint exceeds GPU video-memory budget";
        return r;
    }
    r.reason = "within budget";
    return r;
}

}  // namespace rex::wallpaper
