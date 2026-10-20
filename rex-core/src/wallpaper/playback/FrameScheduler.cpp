#include "wallpaper/playback/FrameScheduler.hpp"

#include <shared/Logger.hpp>

#include <cstdlib>
#include <limits>

namespace rex::wallpaper {

namespace {
constexpr const char* kModule = "FrameScheduler";
// Diagnostics for the freeze/oscillation investigation (P0-1). Starve logs
// are the interesting ones; push logs give the decode-PTS timeline.
inline bool SchedDebug() {
    static const bool v = std::getenv("REX_SCHED_DEBUG") != nullptr;
    return v;
}
}  // namespace

void FrameScheduler::Push(const VideoFrame& frame) {
    const uint64_t wc = m_writeCount.load(std::memory_order_relaxed);
    m_slots[wc % kNumSlots] = frame;
    m_writeCount.store(wc + 1, std::memory_order_release);
    if (SchedDebug()) {
        static uint64_t s_pushLog = 0;
        if ((s_pushLog++ % 600) == 0) {
            REX_LOG_INFO(kModule, "push wc={} pts={:.3f}", wc + 1, frame.pts);
        }
    }
}

const VideoFrame* FrameScheduler::Tick(double targetPts, bool allowCatchUp) {
    const uint64_t wc = m_writeCount.load(std::memory_order_acquire);
    const uint64_t lc =
        m_watermark.load(std::memory_order_acquire) & kWatermarkMask;
    if (wc == 0 || wc == lc) {
        return nullptr;
    }
    // Only the last kNumSlots writes still occupy intact slots. Vanished
    // writes (overwritten unseen) count as dropped, but each exactly
    // once: without the high-water mark every starved Tick re-adds the
    // same stale gap and the counter explodes (14k drops for 586 decoded
    // frames, proven live 2026-10-07).
    uint64_t start = lc + 1;
    const uint64_t oldestIntact = wc > kNumSlots ? wc - kNumSlots + 1 : 1;
    uint64_t countedThrough = m_countedThrough.load(std::memory_order_acquire);
    if (oldestIntact > countedThrough) {
        // Vanished and never accounted: (lo, oldestIntact). Write
        // oldestIntact itself is still intact. Each write counted once
        // via the high-water mark no matter how many starved Ticks pass.
        // (The delta is telemetry; under a Clear race the mark stays
        // forward-only and the re-baseline makes stale math moot.)
        const uint64_t lo = countedThrough > lc ? countedThrough : lc;
        if (oldestIntact > lo + 1) {
            m_dropped.fetch_add(oldestIntact - lo - 1,
                                std::memory_order_relaxed);
        }
        // Forward-only CAS-max (audit 2026-10-17, §5.4 #4): a concurrent
        // Clear() re-baselines the mark to the write count; this stale
        // Tick must never roll it back.
        while (countedThrough < oldestIntact) {
            if (m_countedThrough.compare_exchange_weak(
                    countedThrough, oldestIntact, std::memory_order_release,
                    std::memory_order_acquire)) {
                break;
            }
        }
    }
    if (start < oldestIntact) {
        start = oldestIntact;
    }
    // Newest DUE frame by PTS. No PTS-monotonicity assumption: B-frames decode
    // out of presentation order, so scan everything and keep the latest
    // presentation timestamp that the playback clock has reached.
    //
    // The playback clock is authoritative: publishing a frame does not make it
    // eligible. Frames still in the future are RETAINED for a later Tick.
    //
    // NOTE: no cross-call "PTS must not go backwards" guard. When the decoder
    // falls behind the playhead, the correct catch-up action is to present a
    // LOWER PTS than last time - that is what catching up means. A guard
    // there deadlocks: it rejects every frame once the decoder is behind,
    // the ring fills with un-consumable frames, and the full ring then
    // throttles the decoder further (measured: queue=8, repeat 60/s,
    // convFps 0.61x, effFps 0). Loop-wrap staleness is handled upstream by
    // VideoPlaybackController's IsBackwardJump -> Clear().
    const double limit = targetPts + kSelectToleranceSec;
    uint64_t selected = 0;
    double selectedPts = 0.0;
    bool haveSelection = false;
    uint64_t validCount = 0;
    double minValid = std::numeric_limits<double>::infinity();
    double maxValid = -std::numeric_limits<double>::infinity();
    // The newest VALID frame, whoever it is (it may be in the future).
    uint64_t newestValid = 0;
    double newestValidPts = -std::numeric_limits<double>::infinity();
    for (uint64_t w = start; w <= wc; ++w) {
        const VideoFrame& f = m_slots[(w - 1) % kNumSlots];
        if (!f.valid) {
            continue;
        }
        ++validCount;
        if (f.pts < minValid) minValid = f.pts;
        if (f.pts > maxValid) maxValid = f.pts;
        if (f.pts >= newestValidPts) {
            newestValid = w;
            newestValidPts = f.pts;
        }
        if (f.pts > limit) {
            continue;
        }
        if (!haveSelection || f.pts >= selectedPts) {
            selected = w;
            selectedPts = f.pts;
            haveSelection = true;
        }
    }
    // Backlog recovery: the caller says the published content is more than
    // a second behind the wall clock (the renderer stalled, the ring filled,
    // the decoder blocked). Presenting the newest DUE frame then advances
    // the content only at the RENDERER's rate, so the ring stays full, the
    // decoder stays blocked and the backlog never unwinds - it took ~90 s to
    // recover in the measured case. Presenting the NEWEST published frame
    // jumps the content forward and drains the ring, which unblocks the
    // decoder and lets the reserve recover.
    //
    // Only with the caller's permission: normally the newest published frame
    // is ahead of the playhead (the decode-ahead reserve) and presenting it
    // made the wallpaper read as playing at 2x.
    if (allowCatchUp && haveSelection && newestValidPts > selectedPts) {
        selected = newestValid;
        selectedPts = newestValidPts;
        m_catchUps.fetch_add(1, std::memory_order_relaxed);
    }
    if (!haveSelection) {
        // Nothing is due. The previously displayed frame stays on screen: the
        // render loop re-presents it at wall cadence (Phase 3), so playback
        // stays 1x and the frame is shown for the correct duration.
        //
        // This path deliberately does NOT fall back to "newest published". That
        // behaviour displayed frames whose PTS had not been reached yet, so
        // with a decode-ahead reserve the displayed CONTENT ran ahead of the
        // playhead - the wallpaper read as playing at ~2x while position
        // reported 1.000x and effectiveFps reported the source rate. No
        // telemetry could see it. Loop-wrap starvation is handled upstream by
        // VideoPlaybackController's IsBackwardJump -> Clear(), which re-
        // baselines the ring; starving here is the correct, safe outcome.
        m_starvedTicks.fetch_add(1, std::memory_order_relaxed);
        if (SchedDebug()) {
            static uint64_t s_starveLog = 0;
            const uint64_t n = ++s_starveLog;
            if (n <= 8 || (n % 512) == 0) {
                REX_LOG_WARN(kModule,
                             "starve#{} target={:.3f} limit={:.3f} wc={} lc={} "
                             "pending={} valid={} ptsRange=[{:.3f}..{:.3f}]",
                             n, targetPts, limit, wc, lc,
                             static_cast<uint64_t>(wc - lc), validCount,
                             validCount ? minValid : 0.0,
                             validCount ? maxValid : 0.0);
            }
        }
        return nullptr;
    }
    // Late-frame policy: several frames can already be overdue (decode fell
    // behind the clock). Present the NEWEST overdue frame - that is the one
    // closest to the playhead - and count every older overdue frame that was
    // genuinely skipped. Frames still in the future are untouched.
    uint64_t skipped = 0;
    for (uint64_t w = start; w < selected; ++w) {
        const VideoFrame& f = m_slots[(w - 1) % kNumSlots];
        if (f.valid && f.pts < selectedPts) {
            ++skipped;
        }
    }
    if (skipped) {
        m_dropped.fetch_add(skipped, std::memory_order_relaxed);
    }
    // Fault-injection seam for the Clear-vs-Tick tests (no-op in production).
    if (m_testHook) {
        m_testHook(m_testHookCtx);
    }
    // ── Commit (audit 2026-10-17, §5.4 #4) ─────────────────────────────
    // Advance the watermark FORWARD-ONLY, carrying the epoch read from the
    // same atomic. A Clear() that landed across this scan re-baselined the
    // watermark to (at least) the write count, so this CAS finds
    // lcNow >= selected and the selection is suppressed. The old code
    // stored unconditionally: a stale store could land after the Clear's
    // re-baseline and roll the watermark back ("un-clearing" the ring and
    // re-presenting pre-seek frames).
    uint64_t cur = m_watermark.load(std::memory_order_acquire);
    bool advanced = false;
    for (;;) {
        const uint64_t lcNow = cur & kWatermarkMask;
        if (lcNow >= selected) {
            break;  // a Clear (or an equal commit) owns the watermark now
        }
        const uint64_t next = (cur & ~kWatermarkMask) | selected;
        if (m_watermark.compare_exchange_weak(cur, next,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
            advanced = true;
            break;
        }
        // CAS failure reloaded `cur`; re-check the guard and retry.
    }
    if (!advanced) {
        // Suppress: this scan straddled a Clear. The post-clear ring is
        // empty until the producer pushes again; the consumer re-ticks.
        return nullptr;
    }
    // Burst gate: paired with the epoch carried by this commit (the
    // pre-CAS `cur` value). A later Clear bumps the epoch and disarms the
    // gate atomically — no separate primed store exists to be re-armed
    // out of order.
    m_primedEpoch.store(cur >> kWatermarkBits, std::memory_order_release);
    return &m_slots[(selected - 1) % kNumSlots];
}

void FrameScheduler::Clear() {
    // Publish {epoch+1, watermark = max(wc, current low bits)} as ONE
    // atomic (audit 2026-10-17, §5.4 #4). The packing gives the epoch and
    // the watermark a single modification order, so a racing Tick either
    // sees the old pair (and its forward-only commit CAS loses to, or
    // lands before, this store) or the new pair (and suppresses its stale
    // selection). The epoch bump is also what disarms the burst gate —
    // IsPrimed compares against these top bits — so nothing needs to be
    // stored "after" Clear for the gate to hold.
    const uint64_t wc = m_writeCount.load(std::memory_order_acquire);
    uint64_t cur = m_watermark.load(std::memory_order_acquire);
    for (;;) {
        const uint64_t low = cur & kWatermarkMask;
        const uint64_t target = wc > low ? wc : low;  // never backwards
        const uint64_t next =
            (((cur >> kWatermarkBits) + 1) << kWatermarkBits) | target;
        if (m_watermark.compare_exchange_weak(cur, next,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
            break;
        }
        // CAS failure reloaded `cur`; recompute and retry.
    }
    // Re-baseline the vanished-write high-water mark too, forward-only:
    // a concurrent Tick's stale value must never roll it back.
    uint64_t ct = m_countedThrough.load(std::memory_order_acquire);
    while (ct < wc) {
        if (m_countedThrough.compare_exchange_weak(
                ct, wc, std::memory_order_release,
                std::memory_order_acquire)) {
            break;
        }
    }
}

} // namespace rex::wallpaper
