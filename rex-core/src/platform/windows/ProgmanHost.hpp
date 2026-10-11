#pragma once

#include "IDesktopHost.hpp"
#include "DesktopWindowThread.hpp"

#include <windows.h>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace rex::platform::windows {

class ProgmanHost : public IDesktopHost {
public:
    ProgmanHost();
    ~ProgmanHost() override;

    shared::Result<void> Acquire() override;
    void Release() override;

    /// Show/hide the render window without releasing the layer (P0.1).
    /// Safe before Acquire (no-op when there is no live window).
    void SetVisible(bool visible) override;

    HWND    GetRenderWindow() const override;
    int     GetWidth()  const override;
    int     GetHeight() const override;

    bool    IsValid()    const override;
    bool    IsAcquired() const override;

    std::string_view GetHostType() const override;

    void    StartMonitor(std::function<void()> onRecovery) override;
    void    StopMonitor() override;

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg,
                                     WPARAM wParam, LPARAM lParam);

    void MonitorShell();

    // Same window-thread rule as WorkerWManager (BUG-001): this host's
    // render window must be owned by a thread that dispatches messages, or
    // Explorer blocks on cross-process sends into it. ProgmanHost is the
    // fallback host, active only when WorkerW acquisition fails — but when it
    // is active it owns the desktop and needs the same treatment.
    bool EnsureWindowThread();
    bool RunOnWindowThread(std::function<bool()> op, DWORD timeoutMs = 5000);
    void StopWindowThread();

    HWND m_progman      = nullptr;
    HWND m_renderWindow = nullptr;

    int m_width  = 0;
    int m_height = 0;

    bool m_acquired = false;
    HINSTANCE m_hInstance = nullptr;

    std::atomic<bool>              m_monitoring{false};
    std::thread                    m_monitorThread;
    std::function<void()>          m_recoveryCallback;

    // Owned window thread for the render window (created with the first
    // Acquire, stopped in Release).
    std::unique_ptr<DesktopWindowThread> m_windowThread;
};

} // namespace rex::platform::windows
