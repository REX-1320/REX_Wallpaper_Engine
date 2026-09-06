#include "DesktopWindowThread.hpp"

#include <shared/Logger.hpp>

#include <chrono>

namespace rex::platform::windows {

namespace {

// Completion state shared between Send() and the task it posts. Held via
// shared_ptr so the task stays valid even if Send() returns on timeout
// before the task runs.
struct SendState {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
};

} // namespace

DesktopWindowThread::~DesktopWindowThread() {
    Stop();
}

bool DesktopWindowThread::Start() {
    if (m_running.load(std::memory_order_acquire)) {
        return true;
    }

    m_stopRequested = false;
    m_running.store(true, std::memory_order_release);

    try {
        m_thread = std::thread(&DesktopWindowThread::ThreadProc, this);
    } catch (const std::system_error& ex) {
        REX_LOG_ERROR("DesktopWindowThread", "Failed to start thread: {}",
                      ex.what());
        m_running.store(false, std::memory_order_release);
        return false;
    }

    return m_thread.joinable();
}

void DesktopWindowThread::Stop() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopRequested = true;
    }
    m_cv.notify_all();

    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_running.store(false, std::memory_order_release);
}

void DesktopWindowThread::Post(Task task) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_tasks.push_back(std::move(task));
    }
    m_cv.notify_one();
}

bool DesktopWindowThread::Send(Task task, DWORD timeoutMs) {
    if (IsWindowThread()) {
        task();
        return true;
    }

    auto state = std::make_shared<SendState>();
    Post([state, task = std::move(task)]() {
        task();
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->done = true;
        }
        state->cv.notify_all();
    });

    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                            [&] { return state->done; })) {
        REX_LOG_WARN("DesktopWindowThread",
            "Send timed out after {} ms — task may still be queued", timeoutMs);
        return false;
    }
    return true;
}

void DesktopWindowThread::ThreadProc() {
    m_threadId.store(GetCurrentThreadId(), std::memory_order_release);
    REX_LOG_DEBUG("DesktopWindowThread", "Window thread entered (TID {})",
                  m_threadId.load());

    // Message-only anchor window: gives the thread a real message queue so
    // cross-thread SendMessage calls (from Explorer, or our own hosts) are
    // delivered and dispatched by PumpMessages below.
    constexpr wchar_t kClassName[] = L"REXDesktopWindowThread";
    WNDCLASSW wc = {};
    wc.lpfnWndProc   = MessageWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;

    if (!RegisterClassW(&wc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            REX_LOG_ERROR("DesktopWindowThread",
                "RegisterClassW failed for thread anchor (error: {})", err);
            m_running.store(false, std::memory_order_release);
            return;
        }
    }

    m_messageWindow = CreateWindowExW(
        0, kClassName, L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!m_messageWindow) {
        REX_LOG_ERROR("DesktopWindowThread",
            "Failed to create thread anchor window (error: {})",
            GetLastError());
        m_running.store(false, std::memory_order_release);
        return;
    }

    REX_LOG_DEBUG("DesktopWindowThread",
        "Thread anchor created: {}", fmt::ptr(m_messageWindow));

    while (m_running.load(std::memory_order_acquire)) {
        // 1. Dispatch any messages (Explorer SendMessage to our windows).
        PumpMessages();

        // 2. Run one pending task, then loop back so messages stay pumped
        //    between tasks (never run long window ops without pumping).
        Task task;
        bool stop;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            stop = m_stopRequested;
            if (!m_tasks.empty()) {
                task = std::move(m_tasks.front());
                m_tasks.pop_front();
            }
        }

        if (task) {
            task();
            continue;
        }

        if (stop) {
            break;
        }

        // 3. No work: wait for Post/Stop to wake us (bounded so PumpMessages
        //    still runs at least every 10 ms).
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait_for(lock, std::chrono::milliseconds(10));
    }

    // Drain any tasks still queued (e.g. posted by a timed-out Send that
    // later completed) — safe because they hold shared state / this.
    for (;;) {
        Task task;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_tasks.empty()) break;
            task = std::move(m_tasks.front());
            m_tasks.pop_front();
        }
        if (task) task();
    }

    if (m_messageWindow) {
        DestroyWindow(m_messageWindow);
        m_messageWindow = nullptr;
    }

    m_running.store(false, std::memory_order_release);
    REX_LOG_DEBUG("DesktopWindowThread", "Window thread exiting");
}

void DesktopWindowThread::PumpMessages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        // WM_QUIT is not expected (render windows no longer call
        // PostQuitMessage); Stop() governs thread lifetime.
    }
}

LRESULT CALLBACK DesktopWindowThread::MessageWndProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace rex::platform::windows
