#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace rex::platform::windows {

// ─── DesktopWindowThread ───────────────────────────────────────────────────
//
// Owns every desktop-host HWND (create / reparent / resize / destroy) on a
// dedicated thread that runs a GetMessage pump. Without this, Explorer's
// cross-process SendMessage to a window we own is queued to the thread that
// created that window — and if that thread never dispatches messages (the
// old command/render threads), Explorer blocks forever (AppHangXProcB1).
//
// All window operations in ProgmanHost / WorkerWManager must be marshalled
// here via Send(); the caller blocks only briefly while the window thread
// executes and pumps. Read-only window queries (IsWindow / GetWindowLong /
// GetParent / GetWindow) are safe from any thread and stay off this thread.
//
// The thread is per-host-instance: created in Acquire(), stopped in Release().
// ServiceReacquire reuses the same thread — it is only torn down on a full
// host release.
class DesktopWindowThread {
public:
    using Task = std::function<void()>;

    DesktopWindowThread() = default;
    ~DesktopWindowThread();

    DesktopWindowThread(const DesktopWindowThread&) = delete;
    DesktopWindowThread& operator=(const DesktopWindowThread&) = delete;

    // Start the thread and its message-only anchor window. Safe to call
    // multiple times (no-op once running).
    bool Start();

    // Stop the thread: drains any remaining tasks, then joins. All pending
    // tasks complete before this returns.
    void Stop();

    // Queue a task for the window thread (non-blocking). The task runs with
    // a pumped message loop on the window thread.
    void Post(Task task);

    // Run a task on the window thread and wait for completion. Returns true
    // only when the task actually ran. Never touches the window thread from
    // the window thread itself (runs inline instead of deadlocking).
    bool Send(Task task, DWORD timeoutMs = 3000);

    bool IsRunning() const { return m_running.load(std::memory_order_acquire); }
    DWORD GetThreadId() const { return m_threadId; }

    // True if the calling thread is the window thread.
    bool IsWindowThread() const {
        return GetCurrentThreadId() == m_threadId;
    }

private:
    void ThreadProc();
    void PumpMessages();

    std::thread m_thread;
    std::atomic<DWORD> m_threadId{0};
    std::atomic<bool> m_running{false};
    HWND m_messageWindow = nullptr;   // owned by the window thread only

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<Task> m_tasks;
    bool m_stopRequested = false;     // guarded by m_mutex

    static LRESULT CALLBACK MessageWndProc(HWND hwnd, UINT msg,
                                           WPARAM wParam, LPARAM lParam);
};

} // namespace rex::platform::windows
