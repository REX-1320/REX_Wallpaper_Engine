#pragma once

#include <windows.h>
#include <shared/Result.hpp>
#include <string>
#include <atomic>
#include <thread>
#include <functional>

namespace rex::wallpaper {

using WorkerWRecoveryCallback = std::function<void()>;

class WorkerWManager {
public:
    WorkerWManager();
    ~WorkerWManager();

    shared::Result<void> Acquire();
    void Release();

    /// Start monitoring Explorer.exe and reacquire WorkerW on restart.
    /// The callback is invoked from the monitor thread when the render
    /// window needs to be recreated after an Explorer restart.
    void StartMonitor(WorkerWRecoveryCallback onRecovery);

    /// Stop the Explorer monitor thread (called during shutdown).
    void StopMonitor();

    HWND GetDesktopHwnd()  const { return m_desktopHwnd; }
    HWND GetRenderWindow() const { return m_renderWindow; }
    int  GetWidth()        const { return m_width; }
    int  GetHeight()       const { return m_height; }
    bool IsValid()         const { return m_desktopHwnd != nullptr && m_renderWindow != nullptr; }
    bool IsAcquired()      const { return m_acquired; }

private:
    HWND FindDesktopWindow();
    HWND CreateRenderWindow(HWND parent, int width, int height);
    void ReparentToDesktop(HWND renderWindow, HWND parent);
    void MonitorExplorer();

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND m_desktopHwnd = nullptr;
    HWND m_renderWindow = nullptr;
    int m_width = 0;
    int m_height = 0;
    bool m_acquired = false;

    // Explorer monitor
    std::atomic<bool> m_monitoring{false};
    std::thread m_monitorThread;
    WorkerWRecoveryCallback m_recoveryCallback;
};

} // namespace rex::wallpaper
