#pragma once

// ─── Same-thread frame-lock reentry guard (R1) ──────────────────────────────
//
// BeginFrame() holds m_renderMutex until EndFrame(). Any function that takes
// the same (non-recursive) mutex must never run on the frame owner thread
// inside a frame — it would self-deadlock instead of failing. This policy
// tracks the open-frame window + owner thread so such callers fail loudly;
// cross-thread callers are unaffected and still block normally for exclusion.
//
// Usage: EnterFrame() immediately after acquiring the frame mutex,
// ExitFrame() immediately before releasing it, IsReentrantCall() at the top
// of mutex-taking entry points before they lock.

#include <atomic>

#include <windows.h>

namespace rex::graphics {

class FrameLockPolicy {
public:
    /// Call with the frame mutex already held (BeginFrame, same thread that
    /// will own the frame until EndFrame).
    void EnterFrame() {
        m_frameThreadId.store(::GetCurrentThreadId(), std::memory_order_relaxed);
        m_inFrame.store(true, std::memory_order_release);
    }

    /// Call before releasing the frame mutex (EndFrame and every BeginFrame
    /// early-out that unlocks).
    void ExitFrame() {
        m_inFrame.store(false, std::memory_order_release);
    }

    /// True when the calling thread already owns an open frame: locking the
    /// frame mutex here would self-deadlock. Check before locking.
    bool IsReentrantCall() const {
        return m_inFrame.load(std::memory_order_acquire)
            && m_frameThreadId.load(std::memory_order_relaxed) == ::GetCurrentThreadId();
    }

private:
    std::atomic<bool> m_inFrame{false};
    std::atomic<DWORD> m_frameThreadId{0};
};

}  // namespace rex::graphics
