#pragma once

#include <windows.h>
#include <shared/Result.hpp>
#include <string>
#include <atomic>
#include <mutex>
#include <thread>
#include <functional>

namespace rex::platform::windows {

using WorkerWRecoveryCallback = std::function<void()>;

// ─── Desktop-host lifecycle state (P10 diagnostics) ────────────────────────
// Tracked across Explorer restarts / layer invalidation so crash logs and
// telemetry can answer "was the wallpaper layer healthy?" without guessing.
enum class DesktopHostState {
    Normal,     // acquired, last verification passed
    Suspect,    // an invariant check failed; repair pending or failed
    Repairing,  // ReacquireWorkerW in progress
    Verified,   // repair finished AND VerifyWallpaperLayer passed
};

class WorkerWManager {
public:
    WorkerWManager();
    ~WorkerWManager();

    shared::Result<void> Acquire();
    void Release();

    void StartMonitor(WorkerWRecoveryCallback onRecovery);
    void StopMonitor();

    HWND GetWorkerW()      const { return m_workerW; }
    HWND GetRenderWindow() const { return m_renderWindow; }
    bool IsValid()         const { return m_workerW != nullptr && m_renderWindow != nullptr; }
    bool IsAcquired()      const { return m_acquired; }
    DesktopHostState GetState() const { return m_state.load(std::memory_order_acquire); }

private:
    HWND FindBlankWorkerW();
    HWND CreateRenderWindow(HWND parent, int width, int height);
    void ReparentToWorkerW(HWND renderWindow, HWND workerW);
    void MonitorExplorer();
    bool ReacquireWorkerW();
    bool VerifyWallpaperLayer();

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND m_workerW = nullptr;
    HWND m_renderWindow = nullptr;
    int m_width = 0;
    int m_height = 0;
    int m_virtualLeft = 0;
    int m_virtualTop = 0;
    bool m_acquired = false;
    bool m_syntheticWorkerW = false;

    std::atomic<bool> m_monitoring{false};
    std::thread m_monitorThread;
    // Dedicated mutex for monitor start/stop (NOT m_opMutex: StopMonitor
    // joins the monitor thread, which may be inside ReacquireWorkerW
    // holding m_opMutex — sharing it would deadlock shutdown).
    std::mutex m_monitorMutex;
    WorkerWRecoveryCallback m_recoveryCallback;
    std::atomic<DesktopHostState> m_state{DesktopHostState::Normal};
    // Serializes Acquire/Release/Reacquire. Without it, the monitor
    // thread repairing the layer races the render thread's device-loss
    // reinit (both mutate the same HWNDs) — observed as a permanent
    // mutex-wait hang during Explorer restarts (crash.log 7.6MB of dumps).
    mutable std::mutex m_opMutex;
};

} // namespace rex::platform::windows
