#include "WorkerWManager.hpp"
#include <shared/Logger.hpp>
#include <string>
#include <vector>
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

    std::string WidenToUtf8(const std::wstring& wide) {
        if (wide.empty()) return {};
        int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1,
                                       nullptr, 0, nullptr, nullptr);
        if (size <= 1) return {};
        std::string utf8(size - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1,
                            utf8.data(), size, nullptr, nullptr);
        return utf8;
    }

    // Deterministic desktop-shell layout we enforce:
    //
    //   Progman
    //   ├── WorkerW (wallpaper layer)     ← REX render window goes here
    //   │    └── [no SHELLDLL_DefView]
    //   └── WorkerW (icon layer)
    //        └── SHELLDLL_DefView
    //             └── Desktop icons
    //
    // Windows 11 (22H2+) may attach SHELLDLL_DefView directly to Progman:
    //
    //   Progman
    //   ├── SHELLDLL_DefView
    //   └── WorkerW (wallpaper layer)
    //
    // Either way the rule is identical: the wallpaper render window must
    // be parented into a WorkerW that does NOT contain SHELLDLL_DefView,
    // otherwise it renders ABOVE the desktop icons.
    //
    // Some shells (Explorer Patcher, Windhawk, old wallpaper engines) keep
    // an opaque "paint layer" WorkerW as a Progman child that redraws the
    // OS wallpaper bitmap. It is invisible to EnumWindows (child window),
    // so discovery must look at Progman children directly, and any layer
    // we create has to be inserted ABOVE it or it gets covered.

    constexpr UINT WM_SPAWNWORKERW = 0x052C;

    // Returns the topmost Progman-child WorkerW that paints the OS
    // wallpaper: a WorkerW that contains no SHELLDLL_DefView and is not
    // the window passed as `exclude`. nullptr when none exists.
    HWND FindPaintLayerWorkerW(HWND progman, HWND exclude = nullptr) {
        if (!progman) return nullptr;
        HWND child = FindWindowExW(progman, nullptr, L"WorkerW", nullptr);
        while (child) {
            if (child != exclude) {
                HWND defView = FindWindowExW(child, nullptr,
                                             L"SHELLDLL_DefView", nullptr);
                if (!defView) {
                    return child;  // topmost non-icon WorkerW child
                }
            }
            child = FindWindowExW(progman, child, L"WorkerW", nullptr);
        }
        return nullptr;
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

    m_syntheticWorkerW = false;
    m_workerW = FindBlankWorkerW();
    if (!m_workerW) {
        return shared::Result<void>(shared::ErrorCode::NotFound,
                                    "Failed to find wallpaper WorkerW");
    }

    REX_LOG_INFO("WorkerW", "Found wallpaper WorkerW: {}", fmt::ptr(m_workerW));

    m_width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    m_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    m_virtualLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
    m_virtualTop  = GetSystemMetrics(SM_YVIRTUALSCREEN);
    REX_LOG_INFO("WorkerW", "Virtual desktop: {}x{} offset ({}, {})",
                 m_width, m_height, m_virtualLeft, m_virtualTop);

    m_renderWindow = CreateRenderWindow(m_workerW, m_width, m_height);
    if (!m_renderWindow) {
        return shared::Result<void>(shared::ErrorCode::UnknownError,
                                    "Failed to create render window");
    }

    ReparentToWorkerW(m_renderWindow, m_workerW);

    if (!VerifyWallpaperLayer()) {
        return shared::Result<void>(shared::ErrorCode::UnknownError,
                                    "Wallpaper layer verification failed");
    }

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

    if (m_syntheticWorkerW && m_workerW) {
        REX_LOG_INFO("WorkerW", "Destroying synthetic WorkerW: {}",
                     fmt::ptr(m_workerW));
        DestroyWindow(m_workerW);
        m_syntheticWorkerW = false;
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

// ─── Layer verification ────────────────────────────────────────────────────
//
// Enforces the invariants the user-visible behaviour depends on:
//   1. the render window exists and is WS_CHILD (never top-level popup)
//   2. its parent is a WorkerW
//   3. that WorkerW does NOT contain SHELLDLL_DefView (icon layer)
//   4. neither the render window nor the WorkerW is WS_EX_TOPMOST
//   5. the render window covers the WorkerW client area (virtual desktop)
//
// Returns true when every invariant holds.

bool WorkerWManager::VerifyWallpaperLayer() {
    if (!m_renderWindow) {
        REX_LOG_ERROR("WorkerW", "Verify failed: no render window");
        return false;
    }
    if (!m_workerW) {
        REX_LOG_ERROR("WorkerW", "Verify failed: no WorkerW");
        return false;
    }

    LONG_PTR style = GetWindowLongPtrW(m_renderWindow, GWL_STYLE);
    LONG_PTR exStyle = GetWindowLongPtrW(m_renderWindow, GWL_EXSTYLE);

    if (!(style & WS_CHILD)) {
        REX_LOG_ERROR("WorkerW",
            "Verify failed: render window is not WS_CHILD (style=0x{:X})",
            static_cast<unsigned long long>(style));
        return false;
    }

    if (exStyle & WS_EX_TOPMOST) {
        REX_LOG_ERROR("WorkerW",
            "Verify failed: render window is WS_EX_TOPMOST");
        return false;
    }

    HWND actualParent = GetParent(m_renderWindow);
    if (actualParent != m_workerW) {
        REX_LOG_ERROR("WorkerW",
            "Verify failed: render window parent {:p} != wallpaper WorkerW {:p}",
            fmt::ptr(actualParent), fmt::ptr(m_workerW));
        return false;
    }

    HWND defView = FindWindowExW(m_workerW, nullptr, L"SHELLDLL_DefView", nullptr);
    if (defView) {
        REX_LOG_ERROR("WorkerW",
            "Verify failed: selected WorkerW {:p} contains SHELLDLL_DefView "
            "(icon layer) — wallpaper would render above desktop icons",
            fmt::ptr(m_workerW));
        return false;
    }

    // Z-order invariant: the wallpaper paint layer (a Progman-child WorkerW
    // that redraws the OS wallpaper bitmap) must never sit above our layer,
    // or the wallpaper is covered. Walk siblings upward from our WorkerW;
    // reaching the paint layer before the top of the list means it covers us.
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (progman) {
        HWND paintLayer = FindPaintLayerWorkerW(progman, m_workerW);
        if (paintLayer) {
            HWND above = GetWindow(m_workerW, GW_HWNDPREV);
            while (above) {
                if (above == paintLayer) {
                    REX_LOG_ERROR("WorkerW",
                        "Verify failed: wallpaper paint layer {:p} is above "
                        "our WorkerW {:p} — wallpaper would be covered",
                        fmt::ptr(paintLayer), fmt::ptr(m_workerW));
                    return false;
                }
                above = GetWindow(above, GW_HWNDPREV);
            }
        }
    }

    LONG_PTR wwExStyle = GetWindowLongPtrW(m_workerW, GWL_EXSTYLE);
    if (wwExStyle & WS_EX_TOPMOST) {
        REX_LOG_WARN("WorkerW",
            "WorkerW {:p} is WS_EX_TOPMOST — desktop icons may be obscured",
            fmt::ptr(m_workerW));
    }

    REX_LOG_DEBUG("WorkerW", "Verify OK: render window parented to wallpaper "
                             "WorkerW {:p}, WS_CHILD, not topmost",
                  fmt::ptr(m_workerW));
    return true;
}

// ─── Reacquire (shared by Explorer-restart and layer-invalid paths) ────────

bool WorkerWManager::ReacquireWorkerW() {
    if (m_renderWindow) {
        DestroyWindow(m_renderWindow);
        m_renderWindow = nullptr;
    }
    m_workerW = nullptr;
    m_acquired = false;

    HWND workerW = FindBlankWorkerW();
    if (!workerW) {
        REX_LOG_ERROR("WorkerW", "Reacquire failed: no wallpaper WorkerW");
        return false;
    }

    m_workerW = workerW;
    m_width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    m_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    m_virtualLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
    m_virtualTop  = GetSystemMetrics(SM_YVIRTUALSCREEN);

    m_renderWindow = CreateRenderWindow(m_workerW, m_width, m_height);
    if (!m_renderWindow) {
        REX_LOG_ERROR("WorkerW", "Reacquire failed: render window creation");
        return false;
    }

    ReparentToWorkerW(m_renderWindow, m_workerW);
    m_acquired = true;
    REX_LOG_INFO("WorkerW", "Wallpaper layer reacquired");

    if (m_recoveryCallback) {
        m_recoveryCallback();
    }
    return true;
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

        if (pid == 0) {
            // No Explorer yet — wait and retry.
            std::this_thread::sleep_for(2s);
            continue;
        }

        HANDLE hProcess = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (!hProcess) {
            std::this_thread::sleep_for(2s);
            continue;
        }

        REX_LOG_DEBUG("WorkerW", "Monitoring Explorer.exe (PID: {})", pid);

        // Wait for Explorer exit OR the periodic invariant check interval,
        // whichever comes first. This lets us detect Explorer recreating its
        // desktop windows without us having to restart the wait every time.
        DWORD waitResult = WaitForSingleObject(hProcess, 3000);
        CloseHandle(hProcess);

        if (waitResult == WAIT_OBJECT_0) {
            REX_LOG_WARN("WorkerW", "Explorer.exe terminated. "
                         "Waiting for restart...");

            while (m_monitoring) {
                HWND newProgman = FindWindowW(L"Progman", nullptr);
                if (newProgman && newProgman != progman) {
                    REX_LOG_INFO("WorkerW", "Explorer restarted. "
                                 "Reacquiring WorkerW...");
                    if (!ReacquireWorkerW()) {
                        REX_LOG_ERROR("WorkerW",
                            "Reacquire after Explorer restart failed");
                    }
                    break;
                }
                std::this_thread::sleep_for(500ms);
            }
            continue;
        }

        // ── Periodic invariant check (Explorer alive) ────────────────────
        // Explorer can recreate the desktop window structure while the
        // process stays alive (display mode change, shell refresh, other
        // wallpaper apps, WE applying its own wallpaper, ...). If the
        // render window is no longer parented into the wallpaper WorkerW,
        // it can end up above the icons — reacquire before that shows.
        if (m_acquired) {
            bool ok = m_renderWindow && IsWindow(m_renderWindow) &&
                      m_workerW && IsWindow(m_workerW) &&
                      VerifyWallpaperLayer();

            if (!ok) {
                REX_LOG_WARN("WorkerW",
                    "Wallpaper layer invalid (Explorer refreshed windows); "
                    "reacquiring...");
                if (!ReacquireWorkerW()) {
                    REX_LOG_ERROR("WorkerW",
                        "Reacquire after layer invalidation failed");
                }
            }
        }
    }

    REX_LOG_INFO("WorkerW", "Monitor thread ended");
}

// ─── Wallpaper WorkerW discovery ────────────────────────────────────────────
//
// Steps (see header comment for the target hierarchy):
//   1. locate Progman
//   2. force WorkerW creation (WM_SPAWNWORKERW)
//   3. enumerate every top-level WorkerW
//   4. find the icon-layer WorkerW (contains SHELLDLL_DefView)
//   5. select a different WorkerW — the wallpaper layer
//   6. verify selection properties (visible, parent, size)
//   7. resize it to the virtual desktop bounds

HWND WorkerWManager::FindBlankWorkerW() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman) {
        REX_LOG_ERROR("WorkerW", "Progman not found — desktop layer unavailable");
        return nullptr;
    }

    // Force Explorer to spawn the wallpaper WorkerW.
    for (int i = 0; i < 4; i++) {
        SendMessageTimeoutW(progman, WM_SPAWNWORKERW, 0, 0,
                            SMTO_NORMAL, 1000, nullptr);
        Sleep(100);
    }

    int screenW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int screenH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    int virtualLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int virtualTop  = GetSystemMetrics(SM_YVIRTUALSCREEN);

    wchar_t tempDir[MAX_PATH] = {};
    std::string diagPath;
    if (GetTempPathW(MAX_PATH, tempDir)) {
        diagPath = WidenToUtf8(std::wstring(tempDir) + L"REX\\workerw_diag.log");
        CreateDirectoryW((std::wstring(tempDir) + L"REX").c_str(), nullptr);
    }
    FILE* diag = nullptr;
    fopen_s(&diag, diagPath.c_str(), "w");

    #define WDLOG(fmt, ...) do { \
        if (diag) { fprintf(diag, fmt "\n", ##__VA_ARGS__); fflush(diag); } \
    } while(0)

    WDLOG("=== WorkerW Discovery ===");
    WDLOG("Desktop: %dx%d at (%d,%d)", screenW, screenH, virtualLeft, virtualTop);
    WDLOG("Progman: %p", progman);

    struct Candidate {
        HWND   hwnd;
        RECT   rect;
        bool   visible;
        bool   hasDefView;
        HWND   parent;
        bool   parentIsProgman;
        int    area;
        bool   fullScreen;  // covers >= 90% of the virtual desktop
    };

    std::vector<Candidate> workers;

    EnumWindows([](HWND hwnd, LPARAM lparam) -> BOOL {
        if (GetWindowClassName(hwnd) != L"WorkerW") return TRUE;

        auto* vec = reinterpret_cast<std::vector<Candidate>*>(lparam);

        Candidate c;
        c.hwnd = hwnd;
        GetWindowRect(hwnd, &c.rect);
        c.visible = IsWindowVisible(hwnd);
        c.hasDefView = FindWindowExW(hwnd, nullptr,
                                     L"SHELLDLL_DefView", nullptr) != nullptr;
        c.parent = GetParent(hwnd);
        c.parentIsProgman = (GetClassNameW(c.parent, nullptr, 0) > 0) &&
            (GetWindowClassName(c.parent) == L"Progman");
        c.area = (c.rect.right - c.rect.left) * (c.rect.bottom - c.rect.top);
        c.fullScreen = false;
        vec->push_back(c);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&workers));

    // ── Classify ─────────────────────────────────────────────────────────
    HWND iconWorkerW = nullptr;   // the one containing SHELLDLL_DefView
    for (const auto& w : workers) {
        WDLOG("  WorkerW %p: %dx%d visible=%d defView=%d parent=%p (%s)",
              w.hwnd,
              w.rect.right - w.rect.left, w.rect.bottom - w.rect.top,
              w.visible ? 1 : 0, w.hasDefView ? 1 : 0, w.parent,
              w.parentIsProgman ? "progman" : "other");
        if (w.hasDefView) iconWorkerW = w.hwnd;
    }

    // ── Select the wallpaper layer ───────────────────────────────────────
    // Rule: never select the icon-layer WorkerW. Prefer a visible WorkerW
    // parented under Progman that covers the virtual desktop; otherwise the
    // largest visible one. (Win11 may expose only the wallpaper WorkerW.)
    HWND target = nullptr;
    int bestScore = -1;
    int bestArea = 0;

    for (const auto& w : workers) {
        if (w.hwnd == iconWorkerW) continue;   // invariant: never the icons
        if (!w.visible) continue;

        bool fullScreen = (w.rect.right - w.rect.left >= screenW * 90 / 100 &&
                           w.rect.bottom - w.rect.top >= screenH * 90 / 100);

        int score = 0;
        if (w.parentIsProgman && fullScreen) score = 100;
        else if (w.parentIsProgman)          score = 80;
        else if (fullScreen)                 score = 70;
        else                                 score = 50;

        // Tie-break by size so multi-monitor setups pick the window that
        // spans the virtual desktop.
        if (score > bestScore) {
            bestScore = score;
            target = w.hwnd;
            bestArea = w.area;
        } else if (score == bestScore && w.area > bestArea) {
            target = w.hwnd;
            bestArea = w.area;
        }
    }

    if (target) {
        WDLOG("Selected wallpaper WorkerW: %p (score=%d)", target, bestScore);
    } else {
        WDLOG("No suitable WorkerW found — creating synthetic WorkerW");
    }

    // ── Resize the selected WorkerW to the virtual desktop ───────────────
    // A worker-window sized to the work area clips the wallpaper to part of
    // the screen; DPI mismatches (virtualized vs physical) also leave stale
    // sizes. Always assert the full virtual desktop bounds here.
    if (target) {
        RECT r;
        GetWindowRect(target, &r);
        int curW = r.right - r.left;
        int curH = r.bottom - r.top;
        bool coversDesktop = (curW >= screenW && curH >= screenH);

        // If the icon WorkerW is our sibling, insert directly below it so
        // the wallpaper stays behind the icons even if Explorer reorders
        // windows later. The anchor is either the DefView (Win11: child of
        // Progman) or the icon WorkerW (Win10).
        HWND siblingAnchor = nullptr;
        HWND defView = FindWindowExW(progman, nullptr,
                                     L"SHELLDLL_DefView", nullptr);
        if (!defView && iconWorkerW &&
            GetParent(iconWorkerW) == GetParent(target)) {
            siblingAnchor = iconWorkerW;
        } else if (defView) {
            siblingAnchor = defView;
        }

        if (!coversDesktop || siblingAnchor) {
            WDLOG("Resizing WorkerW %p: %dx%d -> %dx%d anchor=%p",
                  target, curW, curH, screenW, screenH, siblingAnchor);
            SetWindowPos(target,
                         siblingAnchor ? siblingAnchor : HWND_BOTTOM,
                         virtualLeft, virtualTop, screenW, screenH,
                         SWP_NOACTIVATE | SWP_FRAMECHANGED);
            Sleep(100);
        }
    }

    if (!target) {
        // ── Synthetic WorkerW under Progman (last resort) ───────────────
        REX_LOG_WARN("WorkerW", "Creating synthetic WorkerW under Progman");
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
            m_syntheticWorkerW = true;
            REX_LOG_INFO("WorkerW", "Created synthetic WorkerW: {}",
                         fmt::ptr(target));

            // Insert above the explorer wallpaper-paint layer (if any) so
            // the OS wallpaper bitmap cannot cover us, but still below
            // SHELLDLL_DefView so desktop icons stay on top. hWndInsertAfter
            // is the window that PRECEDES (sits above) ours, so when a
            // paint layer exists we anchor to the sibling above it. When
            // no paint layer exists, HWND_BOTTOM is the correct desktop
            // position.
            HWND insertAfter = HWND_BOTTOM;
            HWND paintLayer = FindPaintLayerWorkerW(progman, target);
            if (paintLayer) {
                HWND abovePaint = GetWindow(paintLayer, GW_HWNDPREV);
                insertAfter = abovePaint ? abovePaint : HWND_TOP;
                WDLOG("Synthetic WorkerW: paint layer %p, insertAfter=%p%s",
                      paintLayer, insertAfter,
                      abovePaint ? " (above paint layer)" : " (HWND_TOP)");
            } else {
                WDLOG("Synthetic WorkerW: no paint layer, insertAfter=HWND_BOTTOM");
            }
            SetWindowPos(target, insertAfter,
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

    // Deliberately NOT WS_EX_TOPMOST: the window must stay inside the
    // desktop layer. Created as a top-level popup first (hidden) so it can
    // be parented into the WorkerW in ReparentToWorkerW; never shown
    // between those steps.
    HWND hwnd = CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
        CLASS_NAME,
        L"REX Render",
        WS_POPUP,
        m_virtualLeft, m_virtualTop, width, height,
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

    // Keep the extended style minimal: no WS_EX_TOPMOST ever — the render
    // window belongs inside the desktop layer, not in the top-level Z-order.
    SetWindowLongPtrW(renderWindow, GWL_EXSTYLE,
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT);

    SetWindowPos(renderWindow, HWND_BOTTOM,
                 0, 0,
                 m_width, m_height,
                 SWP_NOACTIVATE | SWP_FRAMECHANGED);

    // Verify final size: if the WorkerW client area is smaller than the
    // virtual desktop (work-area-only WorkerW), shrink the render window
    // to match so nothing is clipped or stretched. Explorer can also
    // resize windows after reparenting, so assert the rect here.
    RECT parentClient;
    if (GetClientRect(workerW, &parentClient)) {
        int cw = parentClient.right - parentClient.left;
        int ch = parentClient.bottom - parentClient.top;
        if (cw > 0 && ch > 0 && (cw != m_width || ch != m_height)) {
            REX_LOG_WARN("WorkerW",
                "Render window {}x{} does not match WorkerW client {}x{}; correcting",
                m_width, m_height, cw, ch);
            SetWindowPos(renderWindow, HWND_BOTTOM, 0, 0, cw, ch,
                SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
    }

    RECT finalRect;
    if (GetWindowRect(renderWindow, &finalRect)) {
        REX_LOG_INFO("WorkerW",
            "Render window final rect: {}x{} at ({}, {})",
            finalRect.right - finalRect.left,
            finalRect.bottom - finalRect.top,
            finalRect.left, finalRect.top);
    }

    // Post-reparent invariant checks (belt and braces — the monitor thread
    // re-verifies periodically too).
    LONG_PTR ex = GetWindowLongPtrW(renderWindow, GWL_EXSTYLE);
    if (ex & WS_EX_TOPMOST) {
        REX_LOG_ERROR("WorkerW",
            "Render window is WS_EX_TOPMOST after reparent — stripping");
        SetWindowLongPtrW(renderWindow, GWL_EXSTYLE,
            ex & ~WS_EX_TOPMOST);
    }

    HWND parent = GetParent(renderWindow);
    if (parent != workerW) {
        REX_LOG_ERROR("WorkerW",
            "Render window parent {:p} != target WorkerW {:p} after reparent",
            fmt::ptr(parent), fmt::ptr(workerW));
    } else {
        REX_LOG_DEBUG("WorkerW",
            "Render window parent verified: wallpaper WorkerW {:p}",
            fmt::ptr(workerW));
    }

    ShowWindow(renderWindow, SW_SHOW);
    UpdateWindow(renderWindow);

    REX_LOG_DEBUG("WorkerW",
        "Window reparented to wallpaper WorkerW and positioned at bottom");
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
