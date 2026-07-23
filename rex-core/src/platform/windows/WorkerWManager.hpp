#pragma once

#include <windows.h>
#include <shared/Result.hpp>
#include <string>
#include <atomic>
#include <thread>
#include <functional>

namespace rex::platform::windows {

using WorkerWRecoveryCallback = std::function<void()>;

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

private:
    HWND FindBlankWorkerW();
    HWND CreateRenderWindow(HWND parent, int width, int height);
    void ReparentToWorkerW(HWND renderWindow, HWND workerW);
    void MonitorExplorer();

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND m_workerW = nullptr;
    HWND m_renderWindow = nullptr;
    int m_width = 0;
    int m_height = 0;
    bool m_acquired = false;

    std::atomic<bool> m_monitoring{false};
    std::thread m_monitorThread;
    WorkerWRecoveryCallback m_recoveryCallback;
};

} // namespace rex::platform::windows
