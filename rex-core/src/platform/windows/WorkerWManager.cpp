#include "WorkerWManager.hpp"
#include <shared/Logger.hpp>
#include <string>
#include <chrono>
#include <thread>
#include <cstdio>

namespace rex::platform::windows {

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

    m_workerW = FindBlankWorkerW();
    if (!m_workerW) {
        return shared::Result<void>(shared::ErrorCode::NotFound,
                                    "Failed to find WorkerW");
    }

    REX_LOG_INFO("WorkerW", "Found WorkerW: {}", fmt::ptr(m_workerW));

    m_width = GetSystemMetrics(SM_CXSCREEN);
    m_height = GetSystemMetrics(SM_CYSCREEN);
    REX_LOG_INFO("WorkerW", "Screen size: {}x{}", m_width, m_height);

    m_renderWindow = CreateRenderWindow(m_workerW, m_width, m_height);
    if (!m_renderWindow) {
        return shared::Result<void>(shared::ErrorCode::UnknownError,
                                    "Failed to create render window");
    }

    REX_LOG_INFO("WorkerW", "Render window created: {}", fmt::ptr(m_renderWindow));

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
        HWND progman = FindWindowW(L"Progman", nullptr);
        DWORD pid = 0;

        if (progman) {
            GetWindowThreadProcessId(progman, &pid);
        }

        if (pid != 0) {
            HANDLE hProcess = OpenProcess(SYNCHRONIZE, FALSE, pid);
            if (hProcess) {
                REX_LOG_DEBUG("WorkerW", "Monitoring Explorer.exe (PID: {})", pid);

                DWORD waitResult = WaitForSingleObject(hProcess, 2000);
                CloseHandle(hProcess);

                if (waitResult == WAIT_OBJECT_0) {
                    REX_LOG_WARN("WorkerW", "Explorer.exe terminated. "
                                 "Waiting for restart...");

                    while (m_monitoring) {
                        HWND newProgman = FindWindowW(L"Progman", nullptr);
                        if (newProgman && newProgman != progman) {
                            REX_LOG_INFO("WorkerW", "Explorer restarted. "
                                         "Reacquiring WorkerW...");

                            if (m_renderWindow) {
                                DestroyWindow(m_renderWindow);
                                m_renderWindow = nullptr;
                            }
                            m_workerW = nullptr;
                            m_acquired = false;

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
                std::this_thread::sleep_for(2s);
            }
        } else {
            std::this_thread::sleep_for(2s);
        }
    }

    REX_LOG_INFO("WorkerW", "Monitor thread ended");
}

HWND WorkerWManager::FindBlankWorkerW() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (progman) {
        for (int i = 0; i < 4; i++) {
            SendMessageTimeoutW(progman, 0x052C, 0, 0,
                                SMTO_NORMAL, 1000, nullptr);
            Sleep(100);
        }
    }

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);

    FILE* diag = nullptr;
    fopen_s(&diag,
        "D:\\REX_doc\\rex-core\\build\\Release\\workerw_diag.log", "w");

    #define WDLOG(fmt, ...) do { \
        if (diag) { fprintf(diag, fmt "\n", ##__VA_ARGS__); fflush(diag); } \
    } while(0)

    WDLOG("=== WorkerW Discovery ===");
    WDLOG("Desktop: %dx%d", screenW, screenH);
    WDLOG("Progman: %p", progman);

    if (progman) {
        LONG_PTR exStyle = GetWindowLongPtrW(progman, GWL_EXSTYLE);
        WDLOG("Progman exStyle: 0x%llX (NOREDIR=%d)",
              (unsigned long long)exStyle,
              (exStyle & 0x00200000) ? 1 : 0);

        HWND childWorkerW = FindWindowExW(progman, nullptr,
                                          L"WorkerW", nullptr);
        HWND defView = FindWindowExW(progman, nullptr,
                                     L"SHELLDLL_DefView", nullptr);
        WDLOG("Progman->WorkerW: %p", childWorkerW);
        WDLOG("Progman->DefView: %p", defView);

        if (childWorkerW) {
            RECT r;
            GetWindowRect(childWorkerW, &r);
            WDLOG("  Child WorkerW: %dx%d visible=%d",
                  r.right - r.left, r.bottom - r.top,
                  IsWindowVisible(childWorkerW) ? 1 : 0);
        }
        if (defView) {
            RECT r;
            GetWindowRect(defView, &r);
            LONG_PTR dvStyle = GetWindowLongPtrW(defView, GWL_EXSTYLE);
            HWND defViewWorkerW = FindWindowExW(defView, nullptr,
                                                L"WorkerW", nullptr);
            WDLOG("  DefView: %dx%d visible=%d layered=%d childWorkerW=%p",
                  r.right - r.left, r.bottom - r.top,
                  IsWindowVisible(defView) ? 1 : 0,
                  (dvStyle & WS_EX_LAYERED) ? 1 : 0,
                  defViewWorkerW);
        }
    }

    HWND bestCandidate = nullptr;
    int   bestScore    = 0;

    struct PollData {
        HWND bestHwnd;
        int  bestScore;
        int  screenW;
        int  screenH;
        int  round;
        FILE* diagFile;
    } pollData;
    pollData.bestHwnd  = nullptr;
    pollData.bestScore = 0;
    pollData.screenW   = screenW;
    pollData.screenH   = screenH;
    pollData.diagFile  = diag;

    for (int pollRound = 0; pollRound < 8; pollRound++) {
        if (pollRound > 0) Sleep(500);
        pollData.round = pollRound;

        if (diag) {
            fprintf(diag, "--- Poll round %d ---\n", pollRound);
            fflush(diag);
        }

        EnumWindows([](HWND hwnd, LPARAM lparam) -> BOOL {
            if (GetWindowClassName(hwnd) != L"WorkerW") return TRUE;

            auto* pd = reinterpret_cast<PollData*>(lparam);

            RECT r;
            GetWindowRect(hwnd, &r);
            int w = r.right - r.left;
            int h = r.bottom - r.top;
            bool visible = IsWindowVisible(hwnd);
            HWND defView = FindWindowExW(hwnd, nullptr,
                L"SHELLDLL_DefView", nullptr);
            HWND parent = GetParent(hwnd);

            bool fullScreen = (w >= pd->screenW * 90 / 100 &&
                               h >= pd->screenH * 90 / 100);
            bool largeEnough = (w >= pd->screenW * 25 / 100 &&
                                h >= pd->screenH * 25 / 100);

            int score = 0;
            if (fullScreen && !defView && visible)      score = 100;
            else if (fullScreen && defView && visible)  score = 80;
            else if (largeEnough && !defView && visible) score = 60;
            else if (largeEnough && defView && visible) score = 40;
            else if (visible)                            score = 20;
            else                                         score = 10;

            if (pd->diagFile) {
                fprintf(pd->diagFile,
                    "  HWND=%p size=%dx%d visible=%d defView=%d "
                    "parent=%p score=%d\n",
                    hwnd, w, h, visible ? 1 : 0,
                    defView ? 1 : 0, parent, score);
                fflush(pd->diagFile);
            }

            if (score > pd->bestScore) {
                pd->bestScore = score;
                pd->bestHwnd  = hwnd;
            }

            return TRUE;
        }, reinterpret_cast<LPARAM>(&pollData));

        bestCandidate = pollData.bestHwnd;
        bestScore     = pollData.bestScore;

        if (bestScore >= 80) {
            if (diag) {
                fprintf(diag, "Good candidate (score=%d), stopping\n",
                        bestScore);
                fflush(diag);
            }
            break;
        }
    }

    HWND target = nullptr;

    if (progman && !target) {
        target = FindWindowExW(progman, nullptr, L"WorkerW", nullptr);
        if (target) {
            RECT r;
            GetWindowRect(target, &r);
            WDLOG("Using child WorkerW of Progman: %p (%dx%d)",
                  target, r.right - r.left, r.bottom - r.top);
            if ((r.right - r.left) < screenW / 2 ||
                (r.bottom - r.top) < screenH / 2) {
                WDLOG("Resizing to %dx%d", screenW, screenH);
                SetWindowPos(target, HWND_BOTTOM,
                    0, 0, screenW, screenH,
                    SWP_NOACTIVATE | SWP_FRAMECHANGED);
                Sleep(100);
            }
        }
    }

    if (!target && progman) {
        HWND defView = FindWindowExW(progman, nullptr,
            L"SHELLDLL_DefView", nullptr);
        if (defView) {
            target = FindWindowExW(defView, nullptr, L"WorkerW", nullptr);
            if (target) {
                WDLOG("Using WorkerW child of DefView: %p", target);
            }
        }
    }

    if (!target && bestCandidate && bestScore >= 80) {
        target = bestCandidate;
        WDLOG("Using full-screen enum WorkerW: %p (score=%d)", target, bestScore);
    }

    if (!target && bestCandidate) {
        target = bestCandidate;
        WDLOG("Resizing WorkerW %p (score=%d) to %dx%d",
              target, bestScore, screenW, screenH);
        SetWindowPos(target, nullptr,
                     0, 0, screenW, screenH,
                     SWP_NOACTIVATE | SWP_NOZORDER | SWP_ASYNCWINDOWPOS);
        Sleep(100);
    }

    if (!target && progman) {
        WDLOG("Creating synthetic WorkerW...");
        WNDCLASSW wc = {};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"WorkerW";
        RegisterClassW(&wc);

        target = CreateWindowExW(
            0, L"WorkerW", L"",
            WS_CHILD | WS_VISIBLE,
            0, 0, screenW, screenH,
            progman, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        if (target) {
            SetWindowPos(target, HWND_BOTTOM,
                0, 0, screenW, screenH,
                SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
    }

    WDLOG("Selected WorkerW: %p", target);
    if (diag) fclose(diag);
    #undef WDLOG

    return target;
}

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

    HWND parent = GetParent(workerW);
    if (parent) {
        HWND defView = FindWindowExW(parent, nullptr,
            L"SHELLDLL_DefView", nullptr);
        if (defView) {
            REX_LOG_INFO("WorkerW",
                "Positioning WorkerW below DefView (Raised Desktop)");
            SetWindowPos(workerW, defView,
                         0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                         SWP_NOOWNERZORDER);
        }
    }

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

} // namespace rex::platform::windows
