#pragma once

#include <windows.h>
#include <shared/Result.hpp>
#include "DesktopWindowThread.hpp"
#include <string>
#include <atomic>
#include <memory>
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

    /// Show/hide the render window without releasing the layer (P0.1).
    /// Hiding lets an OS-drawn static wallpaper show through; the frozen
    /// backbuffer is preserved so re-showing is instant. Safe before
    /// Acquire (no-op when there is no live window). Marshalled to the
    /// window thread like every other mutating HWND op.
    void SetVisible(bool visible);

    void StartMonitor(WorkerWRecoveryCallback onRecovery);
    void StopMonitor();

    HWND GetWorkerW()      const { return m_workerW; }
    HWND GetRenderWindow() const { return m_renderWindow; }    bool IsValid()         const { return m_workerW != nullptr && m_renderWindow != nullptr; }
    bool IsAcquired()      const { return m_acquired; }
    DesktopHostState GetState() const { return m_state.load(std::memory_order_acquire); }
    /// Session/display power state (P3 / G1+G2): set by WndProc from OS
    /// session + console-display notifications on the render window.
    bool IsSessionLocked() const { return m_sessionLocked.load(std::memory_order_acquire); }
    bool IsDisplayOff() const { return m_displayOff.load(std::memory_order_acquire); }

private:
    HWND FindBlankWorkerW(bool requestWorkerWSpawn = true);
    HWND CreateRenderWindow(HWND parent, int width, int height);
    void ReparentToWorkerW(HWND renderWindow, HWND workerW);
    void MonitorExplorer();
    /// Cheap, non-destructive repair: move an alive-but-mispositioned layer
    /// back below the desktop icons. Returns true when the layer verifies
    /// afterwards (no rebuild, no engine reload).
    bool TryReanchorLayer();
    bool ReacquireWorkerW(bool requestWorkerWSpawn = true);
    bool VerifyWallpaperLayer();
    bool VerifyWithRetry(int attempts, int delayMs);
    void DestroySyntheticWorkerW();

    // ─── Window-thread marshalling (BUG-001 / P0-C) ─────────────────────
    // Every MUTATING HWND operation runs here so the thread that owns our
    // windows always dispatches messages. Without it Explorer can block on a
    // cross-process SendMessage to a window owned by the pump-less command or
    // monitor thread, which freezes desktop input (AppHangXProcB1).
    // Read-only queries (IsWindow / GetParent / GetWindowLong / GetWindow)
    // stay on the calling thread — they never reach the owning thread.
    bool EnsureWindowThread();
    bool RunOnWindowThread(std::function<bool()> op, DWORD timeoutMs = 5000);
    void StopWindowThread();

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    /// (Un)register session-lock + console-display notifications on the
    /// render window (P3 / G1+G2). Best-effort: a dead window fails silently.
    void RegisterPowerNotifications();
    void UnregisterPowerNotifications();

    HWND m_workerW = nullptr;
    HWND m_renderWindow = nullptr;
    int m_width = 0;
    int m_height = 0;
    int m_virtualLeft = 0;
    int m_virtualTop = 0;
    bool m_acquired = false;
    bool m_syntheticWorkerW = false;

    // Dedicated owner thread for every desktop HWND we create. Created with
    // the first Acquire(), stopped in Release().
    std::unique_ptr<DesktopWindowThread> m_windowThread;

    std::atomic<bool> m_monitoring{false};
    std::thread m_monitorThread;
    // Dedicated mutex for monitor start/stop (NOT m_opMutex: StopMonitor
    // joins the monitor thread, which may be inside ReacquireWorkerW
    // holding m_opMutex — sharing it would deadlock shutdown).
    std::mutex m_monitorMutex;
    WorkerWRecoveryCallback m_recoveryCallback;
    std::atomic<DesktopHostState> m_state{DesktopHostState::Normal};
    // P3 / G1+G2: set by WndProc from OS notifications; polled by the
    // engine's suspend check (any source pauses, resume when all clear).
    std::atomic<bool> m_sessionLocked{false};
    std::atomic<bool> m_displayOff{false};
    // Power-setting registration handle (returned by Register...; NOT the
    // window — Unregister takes this handle).
    HPOWERNOTIFY m_powerNotify = nullptr;
    // Serializes Acquire/Release/Reacquire. Without it, the monitor
    // thread repairing the layer races the render thread's device-loss
    // reinit (both mutate the same HWNDs) — observed as a permanent
    // mutex-wait hang during Explorer restarts (crash.log 7.6MB of dumps).
    mutable std::mutex m_opMutex;
};

} // namespace rex::platform::windows
