#pragma once

#include "IDesktopHost.hpp"

#include <windows.h>
#include <atomic>
#include <functional>
#include <thread>

namespace rex::platform::windows {

class ProgmanHost : public IDesktopHost {
public:
    ProgmanHost();
    ~ProgmanHost() override;

    shared::Result<void> Acquire() override;
    void Release() override;

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

    HWND m_progman      = nullptr;
    HWND m_renderWindow = nullptr;

    int m_width  = 0;
    int m_height = 0;

    bool m_acquired = false;
    HINSTANCE m_hInstance = nullptr;

    std::atomic<bool>              m_monitoring{false};
    std::thread                    m_monitorThread;
    std::function<void()>          m_recoveryCallback;
};

} // namespace rex::platform::windows
