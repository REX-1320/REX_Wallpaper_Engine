#include "workerw.h"
#include <cstdio>
#include <thread>
#include <chrono>

static constexpr const wchar_t* REX_WINDOW_CLASS = L"REX_RenderWindow";
static constexpr const wchar_t* REX_WINDOW_TITLE = L"REX Core Compositor";

WorkerWHost::WorkerWHost() = default;
WorkerWHost::~WorkerWHost() { Shutdown(); }

bool WorkerWHost::Initialize() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = REX_WINDOW_CLASS;
    RegisterClassExW(&wc);

    HWND workerw = FindRenderTargetWorkerW();
    if (!workerw) {
        printf("[REX] ERROR: Could not find WorkerW target window\n");
        return false;
    }

    if (!CreateRenderWindow(workerw)) {
        printf("[REX] ERROR: Could not create render window inside WorkerW\n");
        return false;
    }

    m_workerW = workerw;
    m_initialized = true;
    printf("[REX] WorkerW host initialized successfully (HWND: %p)\n", m_renderWindow);
    return true;
}

void WorkerWHost::Shutdown() {
    m_initialized = false;
    if (m_explorerHandle) {
        CloseHandle(m_explorerHandle);
        m_explorerHandle = nullptr;
    }
    if (m_renderWindow) {
        DestroyWindow(m_renderWindow);
        m_renderWindow = nullptr;
    }
    UnregisterClassW(REX_WINDOW_CLASS, GetModuleHandleW(nullptr));
}

HWND WorkerWHost::FindRenderTargetWorkerW() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman) {
        printf("[REX] Progman window not found\n");
        return nullptr;
    }

    SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, nullptr);

    HWND targetWorkerW = nullptr;
    EnumWindows([](HWND hwnd, LPARAM lparam) -> BOOL {
        HWND shellView = FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr);
        if (shellView) {
            HWND* result = reinterpret_cast<HWND*>(lparam);
            *result = FindWindowExW(nullptr, hwnd, L"WorkerW", nullptr);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&targetWorkerW));

    return targetWorkerW;
}

bool WorkerWHost::CreateRenderWindow(HWND parentWorkerW) {
    m_renderWindow = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT,
        REX_WINDOW_CLASS,
        REX_WINDOW_TITLE,
        WS_CHILD | WS_VISIBLE,
        0, 0, 1, 1,
        parentWorkerW,
        nullptr,
        GetModuleHandleW(nullptr),
        this
    );

    if (!m_renderWindow) return false;

    SetLayeredWindowAttributes(m_renderWindow, 0, 255, LWA_ALPHA);
    return true;
}

bool WorkerWHost::MonitorExplorer() {
    DWORD explorerPid = 0;

    HWND shellWindow = GetShellWindow();
    if (shellWindow) {
        GetWindowThreadProcessId(shellWindow, &explorerPid);
    }

    if (!explorerPid) return false;

    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, explorerPid);
    if (!process) return false;

    m_explorerHandle = process;

    std::thread([this]() {
        while (m_initialized) {
            DWORD waitResult = WaitForSingleObject(m_explorerHandle, 1000);
            if (waitResult == WAIT_OBJECT_0) {
                printf("[REX] Explorer.exe terminated, waiting for restart...\n");
                CloseHandle(m_explorerHandle);
                m_explorerHandle = nullptr;

                int attempts = 0;
                constexpr int MAX_ATTEMPTS = 10;
                std::chrono::milliseconds delay(100);

                while (attempts < MAX_ATTEMPTS) {
                    std::this_thread::sleep_for(delay);
                    HWND newWorkerW = FindRenderTargetWorkerW();
                    if (newWorkerW) {
                        if (CreateRenderWindow(newWorkerW)) {
                            m_workerW = newWorkerW;
                            printf("[REX] Re-attached to WorkerW after explorer restart\n");
                            if (m_onRestart) m_onRestart();
                        }
                        break;
                    }
                    attempts++;
                    delay = std::min(delay * 2, std::chrono::milliseconds(5000));
                }
            }
        }
    }).detach();

    return true;
}

LRESULT CALLBACK WorkerWHost::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_SIZE:
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
