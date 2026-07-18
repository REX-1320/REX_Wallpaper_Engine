#include "WorkerWManager.hpp"
#include <shared/Logger.hpp>
#include <string>
#include <chrono>
#include <thread>

namespace rex::wallpaper {

namespace {
    std::wstring GetWindowClassName(HWND hwnd) {
        wchar_t buffer[256];
        if (GetClassNameW(hwnd, buffer, 256)) {
            return std::wstring(buffer);
        }
        return L"";
    }
}

WorkerWManager::WorkerWManager() = default;
WorkerWManager::~WorkerWManager() {
    StopMonitor();
    Release();
}

shared::Result<void> WorkerWManager::Acquire() {
    if (m_acquired) {
        return shared::Result<void>(shared::ErrorCode::AlreadyInitialized,
                                    "WorkerW already acquired");
    }

    REX_LOG_INFO("WorkerW", "Acquiring WorkerW...");

    // Find blank WorkerW
    m_workerW = FindBlankWorkerW();
    if (!m_workerW) {
        return shared::Result<void>(shared::ErrorCode::NotFound,
                                    "Failed to find WorkerW");
    }

    REX_LOG_INFO("WorkerW", "Found WorkerW: {}", fmt::ptr(m_workerW));

    // Get screen size
    m_width = GetSystemMetrics(SM_CXSCREEN);
    m_height = GetSystemMetrics(SM_CYSCREEN);
    REX_LOG_INFO("WorkerW", "Screen size: {}x{}", m_width, m_height);

    // Create render window
    m_renderWindow = CreateRenderWindow(m_workerW, m_width, m_height);
    if (!m_renderWindow) {
        return shared::Result<void>(shared::ErrorCode::UnknownError,
                                    "Failed to create render window");
    }

    REX_LOG_INFO("WorkerW", "Render window created: {}", fmt::ptr(m_renderWindow));

    // Reparent to WorkerW
    ReparentToWorkerW(m_renderWindow, m_workerW);

    m_acquired = true;
    REX_LOG_INFO("WorkerW", "WorkerW acquired successfully");
    return shared::Result<void>();
}

void WorkerWManager::Release() {
    if (!m_acquired) return;

    REX_LOG_INFO("WorkerW", "Releasing WorkerW...");

    if (m_renderWindow) {
        DestroyWindow(m_renderWindow);
        m_renderWindow = nullptr;
    }

    m_workerW = nullptr;
    m_acquired = false;
    REX_LOG_INFO("WorkerW", "WorkerW released");
}

void WorkerWManager::StartMonitor(WorkerWRecoveryCallback onRecovery) {
    if (m_monitoring) return;

    m_recoveryCallback = std::move(onRecovery);
    m_monitoring = true;
    m_monitorThread = std::thread(&WorkerWManager::MonitorExplorer, this);

    REX_LOG_INFO("WorkerW", "Explorer monitor started");
}

void WorkerWManager::StopMonitor() {
    if (!m_monitoring) return;
    m_monitoring = false;
    if (m_monitorThread.joinable()) {
        m_monitorThread.join();
    }
    REX_LOG_INFO("WorkerW", "Explorer monitor stopped");
}

void WorkerWManager::MonitorExplorer() {
    using namespace std::chrono_literals;

    REX_LOG_INFO("WorkerW", "Monitor thread started");

    while (m_monitoring) {
        // Get explorer.exe process by window
        HWND progman = FindWindowW(L"Progman", nullptr);
        DWORD pid = 0;

        if (progman) {
            GetWindowThreadProcessId(progman, &pid);
        }

        if (pid != 0) {
            // Open a handle we can wait on
            HANDLE hProcess = OpenProcess(SYNCHRONIZE, FALSE, pid);
            if (hProcess) {
                REX_LOG_DEBUG("WorkerW", "Monitoring Explorer.exe (PID: {})", pid);

                // Wait for explorer.exe to exit or timeout (2s poll interval)
                DWORD waitResult = WaitForSingleObject(hProcess, 2000);
                CloseHandle(hProcess);

                if (waitResult == WAIT_OBJECT_0) {
                    REX_LOG_WARN("WorkerW", "Explorer.exe terminated. "
                                 "Waiting for restart...");

                    // Explorer died. Wait for it to come back.
                    while (m_monitoring) {
                        HWND newProgman = FindWindowW(L"Progman", nullptr);
                        if (newProgman && newProgman != progman) {
                            REX_LOG_INFO("WorkerW", "Explorer restarted. "
                                         "Reacquiring WorkerW...");

                            // Release old state
                            if (m_renderWindow) {
                                DestroyWindow(m_renderWindow);
                                m_renderWindow = nullptr;
                            }
                            m_workerW = nullptr;
                            m_acquired = false;

                            // Re-acquire
                            HWND workerW = FindBlankWorkerW();
                            if (workerW) {
                                m_workerW = workerW;
                                m_width = GetSystemMetrics(SM_CXSCREEN);
                                m_height = GetSystemMetrics(SM_CYSCREEN);

                                m_renderWindow = CreateRenderWindow(
                                    m_workerW, m_width, m_height);
                                if (m_renderWindow) {
                                    ReparentToWorkerW(m_renderWindow, m_workerW);
                                    m_acquired = true;

                                    REX_LOG_INFO("WorkerW",
                                        "WorkerW reacquired after Explorer restart");

                                    // Notify engine to recreate renderer
                                    if (m_recoveryCallback) {
                                        m_recoveryCallback();
                                    }
                                }
                            }
                            break;
                        }
                        std::this_thread::sleep_for(500ms);
                    }
                }
            } else {
                // Can't open process — explorer might be dead already
                std::this_thread::sleep_for(2s);
            }
        } else {
            // Progman not found — explorer is likely dead or not started
            std::this_thread::sleep_for(2s);
        }
    }

    REX_LOG_INFO("WorkerW", "Monitor thread ended");
}

// ─── WorkerW Discovery ──────────────────────────────────────────────────────

HWND WorkerWManager::FindBlankWorkerW() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (progman) {
        REX_LOG_DEBUG("WorkerW", "Found Progman: {}, sending 0x052C",
                      fmt::ptr(progman));
        SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, nullptr);
        Sleep(100);
    } else {
        REX_LOG_WARN("WorkerW", "Progman not found. Is explorer.exe running?");
    }

    HWND target = nullptr;

    // Step 1: Find a blank WorkerW (no SHELLDLL_DefView child)
    EnumWindows([](HWND hwnd, LPARAM lparam) -> BOOL {
        if (GetWindowClassName(hwnd) == L"WorkerW") {
            HWND child = FindWindowExW(hwnd, nullptr,
                                       L"SHELLDLL_DefView", nullptr);
            if (child == nullptr) {
                HWND* result = reinterpret_cast<HWND*>(lparam);
                *result = hwnd;
                REX_LOG_DEBUG("WorkerW", "Found blank WorkerW: {}",
                              fmt::ptr(hwnd));
                return FALSE;
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&target));

    // Step 2: Fallback — sibling method
    if (target == nullptr) {
        REX_LOG_WARN("WorkerW", "Fallback: looking for WorkerW sibling...");
        EnumWindows([](HWND hwnd, LPARAM lparam) -> BOOL {
            if (FindWindowExW(hwnd, nullptr,
                              L"SHELLDLL_DefView", nullptr) != nullptr) {
                HWND* result = reinterpret_cast<HWND*>(lparam);
                *result = FindWindowExW(nullptr, hwnd,
                                        L"WorkerW", nullptr);
                if (*result) {
                    REX_LOG_DEBUG("WorkerW", "Found WorkerW sibling: {}",
                                  fmt::ptr(*result));
                    return FALSE;
                }
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&target));
    }

    if (target == nullptr) {
        REX_LOG_ERROR("WorkerW",
                      "No WorkerW found. Try running as Administrator.");
    }

    return target;
}

// ─── Window Management ─────────────────────────────────────────────────────

HWND WorkerWManager::CreateRenderWindow(HWND parent, int width, int height) {
    const wchar_t CLASS_NAME[] = L"REXRenderWindow";
    HINSTANCE hInst = GetModuleHandleW(nullptr);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = CreateSolidBrush(RGB(0, 0, 0));

    if (!RegisterClassW(&wc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            REX_LOG_ERROR("WorkerW", "RegisterClassW failed: {}", err);
            return nullptr;
        }
    }

    HWND hwnd = CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
        CLASS_NAME,
        L"REX Render",
        WS_POPUP,
        0, 0, width, height,
        nullptr,
        nullptr,
        hInst,
        nullptr);

    if (!hwnd) {
        REX_LOG_ERROR("WorkerW", "CreateWindowExW failed: {}", GetLastError());
        return nullptr;
    }

    return hwnd;
}

void WorkerWManager::ReparentToWorkerW(HWND renderWindow, HWND workerW) {
    if (!renderWindow || !workerW) return;

    SetParent(renderWindow, workerW);

    LONG_PTR style = GetWindowLongPtrW(renderWindow, GWL_STYLE);
    style = (style & ~WS_POPUP) | WS_CHILD | WS_VISIBLE;
    SetWindowLongPtrW(renderWindow, GWL_STYLE, style);

    SetWindowLongPtrW(renderWindow, GWL_EXSTYLE,
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT);

    SetWindowPos(renderWindow, HWND_BOTTOM, 0, 0,
                 m_width, m_height,
                 SWP_NOACTIVATE | SWP_FRAMECHANGED);

    ShowWindow(renderWindow, SW_SHOW);
    UpdateWindow(renderWindow);

    REX_LOG_DEBUG("WorkerW",
                  "Window reparented to WorkerW and positioned at bottom");
}

LRESULT CALLBACK WorkerWManager::WndProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {

    switch (msg) {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

} // namespace rex::wallpaper
