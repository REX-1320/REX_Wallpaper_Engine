#include "WorkerWManager.hpp"
#include <shared/Logger.hpp>
#include <string>
#include <optional>
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

    std::optional<std::pair<int, int>> EnumeratePrimaryMonitorSize() {
        std::optional<std::pair<int, int>> result;

        EnumDisplayMonitors(nullptr, nullptr,
            [](HMONITOR hMon, HDC, LPRECT, LPARAM lParam) -> BOOL {
                auto* out = reinterpret_cast<std::optional<std::pair<int, int>>*>(lParam);
                MONITORINFOEXW mi = {};
                mi.cbSize = sizeof(mi);
                if (GetMonitorInfoW(hMon, &mi) && (mi.dwFlags & MONITORINFOF_PRIMARY)) {
                    *out = std::make_pair(
                        mi.rcMonitor.right - mi.rcMonitor.left,
                        mi.rcMonitor.bottom - mi.rcMonitor.top);
                    return FALSE; // stop enumeration
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&result));

        return result;
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
                                    "Desktop window already acquired");
    }

    REX_LOG_INFO("WorkerW", "Acquiring desktop window...");

    // Find Progman (the desktop window behind icons)
    m_desktopHwnd = FindDesktopWindow();
    if (!m_desktopHwnd) {
        return shared::Result<void>(shared::ErrorCode::NotFound,
                                    "Failed to find Progman desktop window");
    }

    REX_LOG_INFO("WorkerW", "Found Progman: {}", fmt::ptr(m_desktopHwnd));

    // Get primary monitor dimensions from EnumDisplayMonitors
    {
        auto enumResult = EnumeratePrimaryMonitorSize();
        if (enumResult.has_value()) {
            m_width  = enumResult->first;
            m_height = enumResult->second;
        } else {
            // Fallback: use SM_CXSCREEN (now DPI-aware, returns physical pixels)
            m_width  = GetSystemMetrics(SM_CXSCREEN);
            m_height = GetSystemMetrics(SM_CYSCREEN);
        }
    }
    REX_LOG_INFO("WorkerW", "Screen size: {}x{}", m_width, m_height);

    // Create render window
    m_renderWindow = CreateRenderWindow(m_desktopHwnd, m_width, m_height);
    if (!m_renderWindow) {
        return shared::Result<void>(shared::ErrorCode::UnknownError,
                                    "Failed to create render window");
    }

    REX_LOG_INFO("WorkerW", "Render window created: {}", fmt::ptr(m_renderWindow));

    // Reparent to Progman and position behind icons
    ReparentToDesktop(m_renderWindow, m_desktopHwnd);

    m_acquired = true;
    REX_LOG_INFO("WorkerW", "Desktop window acquired successfully");
    return shared::Result<void>();
}

void WorkerWManager::Release() {
    if (!m_acquired) return;

    REX_LOG_INFO("WorkerW", "Releasing desktop window...");

    if (m_renderWindow) {
        DestroyWindow(m_renderWindow);
        m_renderWindow = nullptr;
    }

    m_desktopHwnd = nullptr;
    m_acquired = false;
    REX_LOG_INFO("WorkerW", "Desktop window released");
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
                                         "Reacquiring desktop window...");

                            // Release old state
                            if (m_renderWindow) {
                                DestroyWindow(m_renderWindow);
                                m_renderWindow = nullptr;
                            }
                            m_desktopHwnd = nullptr;
                            m_acquired = false;

                            // Re-acquire
                            HWND progmanHwnd = FindDesktopWindow();
                            if (progmanHwnd) {
                                m_desktopHwnd = progmanHwnd;
                                auto enumResult = EnumeratePrimaryMonitorSize();
                                if (enumResult.has_value()) {
                                    m_width  = enumResult->first;
                                    m_height = enumResult->second;
                                } else {
                                    m_width  = GetSystemMetrics(SM_CXSCREEN);
                                    m_height = GetSystemMetrics(SM_CYSCREEN);
                                }

                                m_renderWindow = CreateRenderWindow(
                                    m_desktopHwnd, m_width, m_height);
                                if (m_renderWindow) {
                                    ReparentToDesktop(m_renderWindow, m_desktopHwnd);
                                    m_acquired = true;

                                    REX_LOG_INFO("WorkerW",
                                        "Desktop reacquired after Explorer restart");

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

// ─── Desktop Window Discovery ─────────────────────────────────────────────

HWND WorkerWManager::FindDesktopWindow() {
    // Use Progman directly — this is the desktop window that sits behind
    // desktop icons (SHELLDLL_DefView). On Windows 11 the blank WorkerW
    // windows are invisible, so reparenting to Progman and placing at
    // HWND_BOTTOM keeps the render window behind the icon layer.
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (progman) {
        REX_LOG_DEBUG("WorkerW", "Found Progman: {}", fmt::ptr(progman));
    } else {
        REX_LOG_ERROR("WorkerW", "Progman not found. Is explorer.exe running?");
    }

    return progman;
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

void WorkerWManager::ReparentToDesktop(HWND renderWindow, HWND desktop) {
    if (!renderWindow || !desktop) return;

    SetParent(renderWindow, desktop);

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
                  "Render window reparented to Progman at HWND_BOTTOM");
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
