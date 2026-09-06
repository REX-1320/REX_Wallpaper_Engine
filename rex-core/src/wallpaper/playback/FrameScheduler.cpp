#include "wallpaper/playback/FrameScheduler.hpp"

namespace rex::wallpaper {

void FrameScheduler::Push(const VideoFrame& frame) {
    const uint64_t wc = m_writeCount.load(std::memory_order_relaxed);
    m_slots[wc % kNumSlots] = frame;
    m_writeCount.store(wc + 1, std::memory_order_release);
}

const VideoFrame* FrameScheduler::Tick(double /*targetPts*/) {
    const uint64_t wc = m_writeCount.load(std::memory_order_acquire);
    if (wc == 0 || wc == m_lastConsumedWrite) {
        return nullptr;
    }
    m_lastConsumedWrite = wc;
    return &m_slots[(wc - 1) % kNumSlots];
}

} // namespace rex::wallpaper
