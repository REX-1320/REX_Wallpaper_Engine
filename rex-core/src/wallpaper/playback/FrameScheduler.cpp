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

const VideoFrame* FrameScheduler::Tick(double targetPts) {
    const uint64_t wc = m_writeCount.load(std::memory_order_acquire);
    const uint64_t lc = m_lastConsumedWrite.load(std::memory_order_acquire);
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
    const uint64_t countedThrough =
        m_countedThrough.load(std::memory_order_acquire);
    if (oldestIntact > countedThrough) {
        // Vanished and never accounted: (lo, oldestIntact). Write
        // oldestIntact itself is still intact. Each write counted once
        // via the high-water mark no matter how many starved Ticks pass.
        const uint64_t lo = countedThrough > lc ? countedThrough : lc;
        if (oldestIntact > lo + 1) {
            m_dropped.fetch_add(oldestIntact - lo - 1,
                                std::memory_order_relaxed);
        }
        m_countedThrough.store(oldestIntact, std::memory_order_release);
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
    for (uint64_t w = start; w <= wc; ++w) {
        const VideoFrame& f = m_slots[(w - 1) % kNumSlots];
        if (!f.valid) {
            continue;
        }
        ++validCount;
        if (f.pts < minValid) minValid = f.pts;
        if (f.pts > maxValid) maxValid = f.pts;
        if (f.pts > limit) {
            continue;
        }
        if (!haveSelection || f.pts >= selectedPts) {
            selected = w;
            selectedPts = f.pts;
            haveSelection = true;
        }
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
                             "pending={} valid={} ptsRange=[{:.3f}..{:.3f}] "
                             "lastShown={:.3f}",
                             n, targetPts, limit, wc, lc,
                             static_cast<uint64_t>(wc - lc), validCount,
                             validCount ? minValid : 0.0,
                             validCount ? maxValid : 0.0, m_lastPresentedPts);
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
    m_lastPresentedPts = selectedPts;
    m_lastConsumedWrite.store(selected, std::memory_order_release);
    return &m_slots[(selected - 1) % kNumSlots];
}

void FrameScheduler::Clear() {
    const uint64_t wc = m_writeCount.load(std::memory_order_acquire);
    m_lastConsumedWrite.store(wc, std::memory_order_release);
    m_countedThrough.store(wc, std::memory_order_release);  // re-baseline
    // Seek / loop wrap / recovery re-bases presentation time. m_lastPresentedPts
    // is NOT cleared here: after a wrap the new frames' PTS restart near zero,
    // and the monotonic guard would reject them all. The consumer's next
    // successful selection re-seeds it (ResetPresentationClock), which the
    // controller calls on the same events that call Clear().
}

} // namespace rex::wallpaper
