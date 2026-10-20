#pragma once

#include "wallpaper/decoder/FfmpegVideoDecoder.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace rex::wallpaper {

// ─── FrameScheduler ─────────────────────────────────────────────────────
//
// SPSC ring of GPU-backed VideoFrames. Exactly one producer (decode thread)
// calls Push(); exactly one consumer (render thread) calls Tick(). Clear()
// and DisarmPrime() are the multi-thread entry points (seek / loop wrap /
// teardown / resume — see the concurrency contract below).
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
// stays memory-valid and no handles are ever closed here. The CONTENT is
// only guaranteed until the producer completes kNumSlots further pushes:
// a consumer that holds a frame across a longer delay must copy it (the
// engine consumes frames BY VALUE at the Tick seam for exactly this reason
// — audit 2026-10-17, §5.4 #14). GPU content safety is guaranteed by the
// D3D11->D3D12 fence: the consumer must wait on GetSlotFence(frame.slot)
// before sampling (see FfmpegVideoDecoder).
//
// ─── Concurrency contract ───────────────────────────────────────────────
//
// Producer vs consumer: slot indices are distinct as long as the producer
// has not lapped the consumer by kNumSlots writes; the write count
// publishes via release/acquire. If multiple producers or consumers are
// ever added, this class MUST be redesigned.
//
// Clear() may run on the command or decode thread WHILE the consumer ticks
// (seek, loop wrap, teardown). The audit-2026-10-17 §5.4 #4 failure modes
// are closed structurally, not by luck: the consumed watermark can never
// move backwards (a Clear's re-baseline used to be overwritten by a racing
// Tick's stale commit — the "un-clear"), and the burst gate is epoch-paired
// inside the SAME atomic as the watermark, so a Clear can never be re-armed
// by a racing consume.

class FrameScheduler {
public:
    static constexpr size_t kNumSlots = 8;
    /// Selection tolerance: a frame up to this far past the wall target
    /// still counts as due (prevents systematic one-frame lateness on
    /// fractional rates like 59.94).
    static constexpr double kSelectToleranceSec = 0.008;

    /// Producer: publish a frame. Overwrites the oldest unpublished slot.
    /// The consumer copies the selected frame out of the ring at the Tick
    /// seam, so a later overwrite cannot tear a selection in progress —
    /// only a consumer that STORES the returned pointer past kNumSlots
    /// further pushes sees newer content in it (see class doc).
    void Push(const VideoFrame& frame);

    /// Consumer: returns the newest published frame due at targetPts
    /// (pts <= targetPts + tolerance), or nullptr when nothing is due.
    /// Skipped and overwritten-unseen frames accumulate in the drop
    /// counter. Never blocks.
    ///
    /// allowCatchUp: the caller says the pipeline is in a BACKLOG (the
    /// published content is more than a second behind the wall clock -
    /// usually because the renderer stalled and the ring filled up). In
    /// that state Tick may present the NEWEST published frame instead of
    /// the newest DUE one, so the content jumps forward and the ring
    /// drains; without it the backlog is an equilibrium that takes tens of
    /// seconds to unwind (measured: reserve -3831 ms for ~90 s).
    /// It is off by default because normally the newest published frame is
    /// AHEAD of the playhead (the decode-ahead reserve), and presenting it
    /// made the wallpaper read as playing at 2x.
    const VideoFrame* Tick(double targetPts, bool allowCatchUp = false);

    /// Cumulative dropped frames (bypassed by selection + overwritten
    /// unseen; equal-PTS repeats excluded). Written by the consumer in
    /// Tick; safe to read from any thread (telemetry).
    uint64_t GetDroppedCount() const {
        return m_dropped.load(std::memory_order_acquire);
    }

    /// Drop any unpublished frame (P1-5 / R10). Called on seek, loop wrap
    /// and recovery resume so a pre-seek frame is never presented: after
    /// Clear, Tick returns nullptr until the next Push. Safe from any
    /// thread (Seek/command thread, decode thread) against a concurrently
    /// ticking consumer — the epoch bump and the watermark re-baseline are
    /// published in ONE atomic (audit §5.4 #4).
    void Clear();

    size_t GetNumSlots() const { return kNumSlots; }

    /// Ticks where nothing was due and the previous frame was retained. With a
    /// decode-ahead reserve this is the NORMAL steady state (the reserve holds
    /// frames ahead of the playhead), so it is a health metric, not an error.
    uint64_t StarvedTicks() const {
        return m_starvedTicks.load(std::memory_order_relaxed);
    }

    /// Published-but-unconsumed frame count now (Phase 1 queue depth).
    /// Clamped to the ring size: anything older was overwritten unseen.
    size_t PendingCount() const {
        const uint64_t wc = m_writeCount.load(std::memory_order_acquire);
        const uint64_t lc =
            m_watermark.load(std::memory_order_acquire) & kWatermarkMask;
        if (wc <= lc) return 0;
        const uint64_t pending = wc - lc;
        return static_cast<size_t>(pending > kNumSlots ? kNumSlots : pending);
    }

    /// True once the consumer has taken a frame since the last Clear.
    /// The decode thread gates catch-up burst on this: bursting into a
    /// fresh (post-seek/wrap/load) ring only manufactures skips — the
    /// playhead starts WITH the content (wallpaper has no lip-sync to
    /// keep), so pace 1x until the first consume, then burst if behind.
    /// Epoch-paired with the watermark, so a racing Clear can never
    /// re-arm the gate (audit §5.4 #4).
    bool IsPrimed() const {
        const uint64_t epoch =
            m_watermark.load(std::memory_order_acquire) >> kWatermarkBits;
        return m_primedEpoch.load(std::memory_order_acquire) == epoch;
    }

    /// Disarm the burst gate WITHOUT dropping buffered frames (resume
    /// path): pace 1x until the next consume re-arms. Unlike Clear, the
    /// pre-pause frames stay consumable — only the sprint is held off.
    void DisarmPrime() {
        m_primedEpoch.store(kNeverPrimed, std::memory_order_release);
    }

    /// Test-only fault-injection seam (audit 2026-10-17, §5.4 #4/#10):
    /// invoked on the consumer thread immediately after a selection and
    /// BEFORE the watermark commit, so tests can inject a Clear() with a
    /// deterministic interleave — timing-based racing is not reliable on
    /// every machine. Always null in production; one predictable branch
    /// per presented frame.
    using BeforeCommitHook = void (*)(void*);
    void SetTestBeforeCommitHook(BeforeCommitHook hook, void* ctx) {
        m_testHook = hook;
        m_testHookCtx = ctx;
    }

private:
    // Consumed watermark + clear epoch, packed into ONE atomic
    // (audit 2026-10-17, §5.4 #4). Bits 63..40: clear epoch, bumped by
    // Clear() from any thread. Bits 39..0: consumed write-count watermark,
    // monotonically non-decreasing — every writer moves it forward-only.
    // The packing gives the pair a single modification order: a Tick
    // commit and a concurrent Clear can never interleave into a torn state
    // (new epoch with the old watermark, or a watermark rolled back past
    // the re-baseline). 2^40 pushes outlive any session; epoch
    // wrap-around is harmless because only same-instant values are ever
    // compared (a Tick lives microseconds).
    static constexpr unsigned kWatermarkBits = 40;
    static constexpr uint64_t kWatermarkMask =
        (uint64_t{1} << kWatermarkBits) - 1;
    // Sentinel for "no consume in the current epoch" (fresh scheduler /
    // DisarmPrime). No real epoch ever reaches it within a tick's lifetime.
    static constexpr uint64_t kNeverPrimed =
        std::numeric_limits<uint64_t>::max();

    std::atomic<uint64_t> m_watermark{0};

    std::array<VideoFrame, kNumSlots> m_slots{};
    std::atomic<uint64_t> m_writeCount{0};
    // Cumulative drops, consumer-written (see Tick), read by telemetry.
    std::atomic<uint64_t> m_dropped{0};
    // High-water mark for vanished-write accounting (see Tick): each
    // overwritten-unseen write is counted at most once, no matter how
    // many starved Ticks observe the same stale gap. Every writer moves
    // it forward-only (CAS-max); Clear re-baselines it to the write count.
    std::atomic<uint64_t> m_countedThrough{0};
    // Epoch of the last COMMITTED consume (see IsPrimed): armed iff it
    // equals the watermark's current epoch. Tick stores the epoch its
    // commit carried; Clear needs no store — the epoch bump alone
    // disarms.
    std::atomic<uint64_t> m_primedEpoch{kNeverPrimed};
    // Ticks that found nothing due (frame retained). Normal with a reserve.
    std::atomic<uint64_t> m_starvedTicks{0};
    // Ticks that jumped the content forward to recover from a backlog (the
    // caller had to allow it - see Tick).
    std::atomic<uint64_t> m_catchUps{0};
    // Test-only seam state (see SetTestBeforeCommitHook); written once by
    // a test before any concurrent use, never touched in production.
    BeforeCommitHook m_testHook = nullptr;
    void* m_testHookCtx = nullptr;
};

} // namespace rex::wallpaper
