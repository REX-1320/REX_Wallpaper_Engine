#pragma once
#include <windows.h>
#include <functional>

class WorkerWHost {
public:
    using EnumCallback = std::function<void()>;

    WorkerWHost();
    ~WorkerWHost();

    bool Initialize();
    void Shutdown();
    HWND GetRenderTargetWindow() const { return m_renderWindow; }

    bool MonitorExplorer();
    void SetOnExplorerRestarted(EnumCallback callback) { m_onRestart = std::move(callback); }

private:
    HWND FindRenderTargetWorkerW();
    bool CreateRenderWindow(HWND parentWorkerW);
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND m_renderWindow = nullptr;
    HWND m_workerW = nullptr;
    HANDLE m_explorerHandle = nullptr;
    EnumCallback m_onRestart;
    bool m_initialized = false;
};
