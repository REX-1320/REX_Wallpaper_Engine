#include "wallpaper/playback/FrameScheduler.hpp"

namespace rex::wallpaper {

void FrameScheduler::Push(const VideoFrame& frame) {
    const uint64_t wc = m_writeCount.load(std::memory_order_relaxed);
    m_slots[wc % kNumSlots] = frame;
    m_writeCount.store(wc + 1, std::memory_order_release);
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
    // Newest due frame. No PTS-monotonicity assumption: B-frames decode
    // out of presentation order, so scan everything and keep the latest
    // decoded frame that is due.
    const double limit = targetPts + kSelectToleranceSec;
    uint64_t selected = 0;
    double selectedPts = 0.0;
    for (uint64_t w = start; w <= wc; ++w) {
        const VideoFrame& f = m_slots[(w - 1) % kNumSlots];
        if (!f.valid || f.pts > limit) {
            continue;
        }
        selected = w;
        selectedPts = f.pts;
    }
    if (selected == 0) {
        // Catch-up fallback (Phase 4): the ring is full of unpublished
        // frames yet nothing is due — the decoder lapped a loop behind
        // the wrapped wall (or surged far ahead), so PTS comparison is
        // phase-broken. Starving here freezes video into repeat/burst
        // cycles (proven live at 2x: 3 uniques/s + 1678 repeats in 30 s
        // while decode ran fine). Show the newest published frame and
        // count the bypassed as dropped instead.
        const uint64_t unconsumed =
            wc > lc ? wc - lc : 0;
        if (unconsumed >= kNumSlots - 1) {
            const VideoFrame& newest = m_slots[(wc - 1) % kNumSlots];
            if (newest.valid) {
                uint64_t skipped = 0;
                for (uint64_t w = start; w < wc; ++w) {
                    const VideoFrame& f = m_slots[(w - 1) % kNumSlots];
                    if (f.valid && f.pts < newest.pts) {
                        ++skipped;
                    }
                }
                m_dropped.fetch_add(skipped, std::memory_order_relaxed);
                m_lastConsumedWrite.store(wc, std::memory_order_release);
                return &m_slots[(wc - 1) % kNumSlots];
            }
        }
        return nullptr;  // everything published is still future
    }
    // Bypassed frames with strictly older PTS count as dropped;
    // equal-PTS repeats are free (same instant, not a skip).
    uint64_t skipped = 0;
    for (uint64_t w = start; w < selected; ++w) {
        const VideoFrame& f = m_slots[(w - 1) % kNumSlots];
        if (f.valid && f.pts < selectedPts) {
            ++skipped;
        }
    }
    m_dropped.fetch_add(skipped, std::memory_order_relaxed);
    m_lastConsumedWrite.store(selected, std::memory_order_release);
    return &m_slots[(selected - 1) % kNumSlots];
}

void FrameScheduler::Clear() {
    const uint64_t wc = m_writeCount.load(std::memory_order_acquire);
    m_lastConsumedWrite.store(wc, std::memory_order_release);
    m_countedThrough.store(wc, std::memory_order_release);  // re-baseline
}

} // namespace rex::wallpaper
