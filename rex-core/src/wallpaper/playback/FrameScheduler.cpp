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
    // Only the last kNumSlots writes still occupy intact slots; anything
    // older was overwritten unseen — count it dropped.
    uint64_t start = lc + 1;
    const uint64_t oldestIntact = wc > kNumSlots ? wc - kNumSlots + 1 : 1;
    if (start < oldestIntact) {
        m_dropped.fetch_add(oldestIntact - start, std::memory_order_relaxed);
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
    m_lastConsumedWrite.store(m_writeCount.load(std::memory_order_acquire),
                              std::memory_order_release);
}

} // namespace rex::wallpaper
