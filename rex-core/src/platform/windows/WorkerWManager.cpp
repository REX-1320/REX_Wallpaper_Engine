#include "WorkerWManager.hpp"
#include "LayerRepairPolicy.hpp"
#include <shared/Logger.hpp>
#include <wtsapi32.h>
#include <powrprof.h>
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

// ─── Window-thread marshalling (BUG-001 / P0-C) ─────────────────────────────

bool WorkerWManager::EnsureWindowThread() {
    if (m_windowThread && m_windowThread->IsRunning()) return true;
    if (!m_windowThread) {
        m_windowThread = std::make_unique<DesktopWindowThread>();
    }
    if (m_windowThread->Start()) {
        REX_LOG_INFO("WorkerW", "Desktop window thread started (TID {})",
                     m_windowThread->GetThreadId());
        return true;
    }
    REX_LOG_ERROR("WorkerW", "Failed to start desktop window thread — "
                             "window ops will run on the calling thread");
    m_windowThread.reset();
    return false;
}

bool WorkerWManager::RunOnWindowThread(std::function<bool()> op, DWORD timeoutMs) {
    if (!m_windowThread || !m_windowThread->IsRunning()) {
        REX_LOG_WARN("WorkerW",
                     "Window thread unavailable; running window op inline");
        return op();
    }
    if (m_windowThread->Send(std::move(op), timeoutMs)) return true;
    // Timeout: the task may still be queued and will run later, so the caller
    // must treat this as a failure and re-verify rather than assume state.
    REX_LOG_ERROR("WorkerW", "Window thread task did not complete within {} ms",
                  timeoutMs);
    return false;
}

shared::Result<void> WorkerWManager::Acquire() {
    std::lock_guard<std::mutex> opLock(m_opMutex);
    if (m_acquired) {
        return shared::Result<void>(shared::ErrorCode::AlreadyInitialized,
                                    "WorkerW already acquired");
    }

    REX_LOG_INFO("WorkerW", "Acquiring WorkerW...");

    EnsureWindowThread();

    m_syntheticWorkerW = false;
    // Discovery can cost seconds (WM_SPAWNWORKERW with 1 s timeouts) — give
    // the window thread room so a slow-but-successful discovery is not
    // misreported as a failure.
    if (!RunOnWindowThread([this] { m_workerW = FindBlankWorkerW(true); return true; },
                           10000)) {
        m_workerW = nullptr;
        return shared::Result<void>(shared::ErrorCode::NotFound,
                                    "WorkerW discovery did not complete");
    }
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

    // Create + reparent in one window-thread task so the render window is
    // never parented by a different thread than the one that created it.
    const bool created = RunOnWindowThread([this] {
        m_renderWindow = CreateRenderWindow(m_workerW, m_width, m_height);
        if (!m_renderWindow) return false;
        ReparentToWorkerW(m_renderWindow, m_workerW);
        return true;
    });
    if (!created || !m_renderWindow) {
        RunOnWindowThread([this] {
            if (m_renderWindow) DestroyWindow(m_renderWindow);
            m_renderWindow = nullptr;
            return true;
        });
        m_workerW = nullptr;
        m_state.store(DesktopHostState::Suspect, std::memory_order_release);
        return shared::Result<void>(shared::ErrorCode::UnknownError,
                                    "Failed to create render window");
    }

    if (!VerifyWithRetry(5, 300)) {
        // Tear down what we just built: leaving a render window behind on
        // a failed acquire leaks an HWND per attempt and can leave a
        // stray visible layer over the desktop.
        RunOnWindowThread([this] {
            if (m_renderWindow) {
                DestroyWindow(m_renderWindow);
                m_renderWindow = nullptr;
            }
            return true;
        });
        DestroySyntheticWorkerW();
        m_workerW = nullptr;
        m_state.store(DesktopHostState::Suspect, std::memory_order_release);
        return shared::Result<void>(shared::ErrorCode::UnknownError,
                                    "Wallpaper layer verification failed");
    }

    m_acquired = true;
    m_state.store(DesktopHostState::Normal, std::memory_order_release);
    REX_LOG_INFO("WorkerW", "WorkerW acquired successfully");
    // P3 / G1+G2: session + display notifications for the suspend check.
    RegisterPowerNotifications();
    return shared::Result<void>();
}

void WorkerWManager::Release() {
    std::lock_guard<std::mutex> opLock(m_opMutex);
    if (!m_acquired) {
        // Nothing acquired, but a previous attempt may still own windows.
        if (m_renderWindow || m_syntheticWorkerW) {
            RunOnWindowThread([this] {
                if (m_renderWindow) {
                    DestroyWindow(m_renderWindow);
                    m_renderWindow = nullptr;
                }
                return true;
            });
            DestroySyntheticWorkerW();
            m_workerW = nullptr;
        }
        StopWindowThread();
        return;
    }

    REX_LOG_INFO("WorkerW", "Releasing WorkerW...");

    // P3 / G1+G2: drop notifications while the window is still alive.
    UnregisterPowerNotifications();

    RunOnWindowThread([this] {
        if (m_renderWindow) {
            DestroyWindow(m_renderWindow);
            m_renderWindow = nullptr;
        }
        return true;
    });

    if (m_syntheticWorkerW && m_workerW) {
        REX_LOG_INFO("WorkerW", "Destroying synthetic WorkerW: {}",
                     fmt::ptr(m_workerW));
        DestroySyntheticWorkerW();
    }

    m_workerW = nullptr;
    m_acquired = false;
    REX_LOG_INFO("WorkerW", "WorkerW released");

    // The window thread is torn down only on a full release — reacquire and
    // re-anchor keep using it, so Explorer always finds a thread that
    // dispatches messages for our windows.
    StopWindowThread();
}

// ─── Render-window visibility (P0.1: video→image frozen-frame cover) ─────
// Hide the layer without releasing it so an OS-drawn static wallpaper shows
// through; show restores it instantly with the backbuffer intact.
void WorkerWManager::SetVisible(bool visible) {
    HWND hwnd = m_renderWindow;
    if (!hwnd || !IsWindow(hwnd)) return;  // pre-Acquire or released: no-op
    RunOnWindowThread([hwnd, visible] {
        if (IsWindow(hwnd)) {
            ShowWindow(hwnd, visible ? SW_SHOW : SW_HIDE);
        }
        return true;
    });
}

// ─── Non-destructive repair (BUG-001) ──────────────────────────────────────
//
// A layer that Explorer merely pushed above the desktop icons is still alive
// and still the right window — it only needs to move back down the z-order.
// The previous behaviour (destroy + recreate + WM_SPAWNWORKERW storm + engine
// reload) is what produced the visible jump above the icons and the ~1 s of
// frozen desktop input.

bool WorkerWManager::TryReanchorLayer() {
    if (!m_renderWindow || !IsWindow(m_renderWindow) ||
        !m_workerW || !IsWindow(m_workerW)) {
        return false;
    }

    // Anchor directly below the icon layer when we can identify it, so the
    // wallpaper ends up behind the icons instead of merely at the bottom.
    HWND anchor = HWND_BOTTOM;
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (progman) {
        HWND defView = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);
        if (defView) {
            anchor = defView;
        } else {
            for (HWND child = GetWindow(progman, GW_CHILD); child;
                 child = GetWindow(child, GW_HWNDNEXT)) {
                wchar_t cls[32] = {};
                GetClassNameW(child, cls, 32);
                if (lstrcmpW(cls, L"WorkerW") == 0 &&
                    FindWindowExW(child, nullptr, L"SHELLDLL_DefView", nullptr)) {
                    anchor = child;
                    break;
                }
            }
        }
    }

    REX_LOG_INFO("WorkerW", "Re-anchoring wallpaper layer {:p} below anchor {:p}",
                 fmt::ptr(m_workerW), fmt::ptr(anchor));
    // Marshalled: SetWindowPos across processes must run on the thread that
    // owns our windows.
    const bool moved = RunOnWindowThread([this, anchor] {
        SetWindowPos(m_workerW, anchor, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        SetWindowPos(m_renderWindow, HWND_BOTTOM, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return true;
    });
    if (!moved) return false;

    if (!VerifyWallpaperLayer()) {
        REX_LOG_WARN("WorkerW", "Re-anchor did not verify; a rebuild is required");
        return false;
    }
    return true;
}

void WorkerWManager::DestroySyntheticWorkerW() {
    if (!m_syntheticWorkerW) return;
    // Marshalled: the synthetic layer is owned by the window thread. A call
    // from inside a window-thread task runs inline (DesktopWindowThread::Send
    // detects that), so this never self-deadlocks.
    RunOnWindowThread([this] {
        if (m_workerW && IsWindow(m_workerW)) {
            REX_LOG_INFO("WorkerW", "Destroying synthetic WorkerW: {}",
                         fmt::ptr(m_workerW));
            DestroyWindow(m_workerW);
        }
        m_syntheticWorkerW = false;
        m_workerW = nullptr;
        return true;
    });
}

void WorkerWManager::StopWindowThread() {
    if (!m_windowThread) return;
    m_windowThread->Stop();
    m_windowThread.reset();
}

void WorkerWManager::StartMonitor(WorkerWRecoveryCallback onRecovery) {
    std::lock_guard<std::mutex> lock(m_monitorMutex);
    if (m_monitoring) return;

    m_recoveryCallback = std::move(onRecovery);
    m_monitoring = true;
    m_monitorThread = std::thread(&WorkerWManager::MonitorExplorer, this);

    REX_LOG_INFO("WorkerW", "Explorer monitor started");
}

void WorkerWManager::StopMonitor() {
    {
        std::lock_guard<std::mutex> lock(m_monitorMutex);
        if (!m_monitoring) return;
        m_monitoring = false;
    }
    // Join outside m_monitorMutex AND m_opMutex: the monitor thread may be
    // inside ReacquireWorkerW holding m_opMutex (bounded by its message
    // timeouts); joining while holding either lock would deadlock.
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

    // Our own synthetic layer must never be able to eat desktop input. It is
    // created transparent/no-activate; if a future edit drops those styles
    // the symptom is a dead desktop whenever the shell reorders us above the
    // icons, so treat it as a hard invariant.
    if (m_syntheticWorkerW) {
        const LONG_PTR synthEx = GetWindowLongPtrW(m_workerW, GWL_EXSTYLE);
        if ((synthEx & WS_EX_TRANSPARENT) == 0 || (synthEx & WS_EX_NOACTIVATE) == 0) {
            REX_LOG_ERROR("WorkerW",
                "Verify failed: synthetic WorkerW {:p} is not input-transparent "
                "(exstyle=0x{:X}) — it would swallow desktop clicks while above the icons",
                fmt::ptr(m_workerW), static_cast<unsigned long long>(synthEx));
            return false;
        }
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

        // Z-order invariant: OUR layer must sit strictly BELOW the icon
        // layer. A wallpaper window above SHELLDLL_DefView captures all
        // desktop clicks (dead icons, broken shortcuts, taskbar hover
        // issues) while every other check above still passes. Only
        // enforceable when both share Progman as parent; otherwise warn.
        HWND iconLayer = nullptr;
        for (HWND child = GetWindow(progman, GW_CHILD); child;
             child = GetWindow(child, GW_HWNDNEXT)) {
            wchar_t cls[32] = {};
            GetClassNameW(child, cls, 32);
            if (lstrcmpW(cls, L"WorkerW") == 0 &&
                FindWindowExW(child, nullptr, L"SHELLDLL_DefView", nullptr)) {
                iconLayer = child;
                break;
            }
        }
        // Fallback: DefView isn't always a direct Progman child (nested
        // hosts, remote sessions) — and FindWindowEx(NULL) only searches
        // top-level windows, so a nested DefView is invisible to it.
        // Deep-search the whole window tree instead.
        HWND iconParent = nullptr;
        if (!iconLayer) {
            HWND defView = nullptr;
            EnumWindows(
                [](HWND hwnd, LPARAM lparam) -> BOOL {
                    HWND* out = reinterpret_cast<HWND*>(lparam);
                    if (*out) return FALSE;
                    wchar_t cls[32] = {};
                    GetClassNameW(hwnd, cls, 32);
                    if (lstrcmpW(cls, L"SHELLDLL_DefView") == 0) {
                        *out = hwnd;
                        return FALSE;
                    }
                    EnumChildWindows(hwnd,
                        [](HWND child, LPARAM inner) -> BOOL {
                            HWND* o = reinterpret_cast<HWND*>(inner);
                            if (*o) return FALSE;
                            wchar_t c2[32] = {};
                            GetClassNameW(child, c2, 32);
                            if (lstrcmpW(c2, L"SHELLDLL_DefView") == 0) {
                                *o = child;
                                return FALSE;
                            }
                            return TRUE;
                        },
                        lparam);
                    return *out == nullptr;
                },
                reinterpret_cast<LPARAM>(&defView));
            if (defView) {
                iconParent = GetParent(defView);
                HWND resolved = iconParent;
                if (resolved) {
                    for (HWND w = m_workerW; w; w = GetParent(w)) {
                        if (w == resolved) { resolved = defView; break; }
                    }
                }
                if (resolved && resolved != m_workerW) {
                    iconLayer = resolved;
                    wchar_t cls[32] = {};
                    GetClassNameW(iconLayer, cls, 32);
                    REX_LOG_DEBUG("WorkerW",
                        "Verify: icon layer resolved via DefView: {:p} (class {})",
                        fmt::ptr(iconLayer),
                        WidenToUtf8(cls).c_str());
                }
            }
        }
        if (iconLayer) {
            // General ordering via lowest common ancestor: climb both
            // chains to the desktop, find where they diverge, then require
            // our branch to sit strictly below the icon branch. Works no
            // matter which window classes Explorer nests the layers under
            // (WorkerW, CtrlNotifySink, ...).
            auto chainToDesktop = [](HWND h) {
                std::vector<HWND> chain;
                for (HWND w = h; w && w != GetDesktopWindow() && chain.size() < 64;
                     w = GetParent(w)) {
                    chain.push_back(w);
                }
                return chain;
            };
            const auto ourChain = chainToDesktop(m_workerW);
            const auto iconChain = chainToDesktop(iconLayer);
            // Deepest common window = lowest common ancestor.
            size_t oi = ourChain.size(), ii = iconChain.size();
            while (oi > 0 && ii > 0 && ourChain[oi - 1] == iconChain[ii - 1]) {
                --oi;
                --ii;
            }
            if (oi == 0 || ii == 0) {
                // One contains the other — we must NOT be inside (or be)
                // the icon layer itself.
                REX_LOG_ERROR("WorkerW",
                    "Verify failed: our WorkerW {:p} and icon layer {:p} "
                    "are nested — video would cover desktop icons",
                    fmt::ptr(m_workerW), fmt::ptr(iconLayer));
                return false;
            }
            HWND ourTop = ourChain[oi - 1];
            HWND iconTop = iconChain[ii - 1];
            bool belowIcons = false;
            for (HWND w = GetWindow(iconTop, GW_HWNDNEXT); w;
                 w = GetWindow(w, GW_HWNDNEXT)) {
                if (w == ourTop) {
                    belowIcons = true;
                    break;
                }
            }
            if (!belowIcons) {
                REX_LOG_ERROR("WorkerW",
                    "Verify failed: our layer {:p} is at/above the icon "
                    "layer {:p} — video would cover desktop icons and eat input",
                    fmt::ptr(ourTop), fmt::ptr(iconTop));
                return false;
            }
            REX_LOG_DEBUG("WorkerW",
                "Verify: our layer {:p} is below icon layer {:p}",
                fmt::ptr(ourTop), fmt::ptr(iconTop));
        } else {
            REX_LOG_WARN("WorkerW",
                "Verify: no icon layer found; skipping below-icons check "
                "(icons may be hidden)");
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

bool WorkerWManager::VerifyWithRetry(int attempts, int delayMs) {
    if (attempts < 1) attempts = 1;
    for (int i = 0; i < attempts; ++i) {
        if (i > 0 && delayMs > 0) Sleep(static_cast<DWORD>(delayMs));
        if (VerifyWallpaperLayer()) return true;
    }
    return false;
}

bool WorkerWManager::ReacquireWorkerW(bool requestWorkerWSpawn) {
    // The recovery callback (engine reload) may call back into
    // Acquire/Release, so it must run WITHOUT m_opMutex held. Everything
    // below up to the unlock scope runs under the lock; the callback runs
    // after it.
    WorkerWRecoveryCallback callback;
    bool repaired = false;
    {
        std::lock_guard<std::mutex> opLock(m_opMutex);
        m_state.store(DesktopHostState::Repairing, std::memory_order_release);
        REX_LOG_INFO("WorkerW", "Repairing wallpaper layer (reacquire)...");
        EnsureWindowThread();

        // P3 / G1+G2: drop notifications for the dying window first (its
        // HPOWERNOTIFY would otherwise leak; re-registered below on the new
        // window after verification).
        UnregisterPowerNotifications();

        // Destroy on the owning thread (the monitor thread does not pump).
        RunOnWindowThread([this] {
            if (m_renderWindow) {
                DestroyWindow(m_renderWindow);
                m_renderWindow = nullptr;
            }
            return true;
        });
        // P0-1: a previous synthetic layer is OUR window — destroy it before
        // discovery, and reset the flag so it follows the new discovery
        // (FindBlankWorkerW sets it when it creates one). A stale `true`
        // across a synthetic -> real transition makes VerifyWallpaperLayer
        // demand TRANSPARENT|NOACTIVATE on Explorer's own WorkerW, failing
        // every repair forever.
        if (PlanSyntheticTransition(m_syntheticWorkerW, false).destroyOld) {
            DestroySyntheticWorkerW();
        } else {
            m_workerW = nullptr;
        }
        m_acquired = false;

        HWND workerW = nullptr;
        if (!RunOnWindowThread([this, &workerW, requestWorkerWSpawn] {
                workerW = FindBlankWorkerW(requestWorkerWSpawn);
                return true;
            }, 10000) || !workerW) {
            m_state.store(DesktopHostState::Suspect, std::memory_order_release);
            REX_LOG_ERROR("WorkerW", "Reacquire failed: no wallpaper WorkerW");
            return false;
        }

        m_workerW = workerW;
        m_width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        m_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        m_virtualLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
        m_virtualTop = GetSystemMetrics(SM_YVIRTUALSCREEN);

        const bool created = RunOnWindowThread([this] {
            m_renderWindow = CreateRenderWindow(m_workerW, m_width, m_height);
            if (!m_renderWindow) return false;
            ReparentToWorkerW(m_renderWindow, m_workerW);
            return true;
        });
        if (!created || !m_renderWindow) {
            m_workerW = nullptr;
            m_state.store(DesktopHostState::Suspect, std::memory_order_release);
            REX_LOG_ERROR("WorkerW", "Reacquire failed: render window creation");
            return false;
        }

        // P10: verify BEFORE declaring success — the monitor loop treats a
        // true return as healthy, so an unverified repair could leave the
        // render window above the icons for a full check interval. The
        // icon topology is transiently wrong right after an Explorer restart
        // (DefView is briefly a direct Progman child before it is re-nested),
        // so allow a few settled attempts before declaring the layer unusable.
        if (!VerifyWithRetry(5, 300)) {
            RunOnWindowThread([this] {
                if (m_renderWindow) {
                    DestroyWindow(m_renderWindow);
                    m_renderWindow = nullptr;
                }
                return true;
            });
            DestroySyntheticWorkerW();
            m_workerW = nullptr;
            m_acquired = false;
            m_state.store(DesktopHostState::Suspect, std::memory_order_release);
            REX_LOG_ERROR("WorkerW", "Reacquire failed: post-repair verification");
            return false;
        }

        m_acquired = true;
        m_state.store(DesktopHostState::Verified, std::memory_order_release);
        REX_LOG_INFO("WorkerW", "Wallpaper layer reacquired and verified");
        // P3 / G1+G2: the render window is new — re-register notifications.
        RegisterPowerNotifications();
        callback = m_recoveryCallback;
        repaired = true;
    } // opLock releases here.

    if (repaired && callback) {
        callback();
    }
    return repaired;
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
        // wallpaper apps, WE applying its own wallpaper, ...). A layer that
        // ended up above the icons must be repaired — but a mispositioned
        // layer only needs to move, not to be destroyed and rebuilt.
        // A previously FAILED repair leaves m_acquired false; that state must
        // also retry, otherwise one transient failure costs the wallpaper
        // permanently (the engine keeps decoding into a destroyed window and
        // the desktop silently reverts to Explorer's own wallpaper).
        bool needsRepair = true;
        bool requestSpawn = true;
        if (m_acquired) {
            const bool renderAlive = m_renderWindow && IsWindow(m_renderWindow);
            const bool workerAlive  = m_workerW && IsWindow(m_workerW);
            const bool layerValid   = renderAlive && workerAlive &&
                                      VerifyWallpaperLayer();
            // WM_SPAWNWORKERW only when a window is actually gone.
            requestSpawn = NeedsWorkerWSpawn(renderAlive && workerAlive);

            const LayerRepair repair =
                ChooseLayerRepair(renderAlive, workerAlive, layerValid, false);
            if (repair == LayerRepair::Reanchor) {
                if (TryReanchorLayer()) {
                    // Cheap path taken: no destroy, no recreate, no engine
                    // reload — the desktop never sees the layer disappear.
                    REX_LOG_INFO("WorkerW",
                                 "Wallpaper layer re-anchored below the icons");
                    m_state.store(DesktopHostState::Verified,
                                  std::memory_order_release);
                    continue;
                }
                REX_LOG_WARN("WorkerW",
                             "Layer invalid and re-anchor failed; reacquiring...");
                needsRepair = true;
            } else {
                needsRepair = (repair != LayerRepair::None);
            }
        }

        if (needsRepair) {
            m_state.store(DesktopHostState::Suspect, std::memory_order_release);
            if (m_acquired) {
                REX_LOG_WARN("WorkerW",
                             "Wallpaper layer invalid (Explorer refreshed windows); "
                             "reacquiring...");
            } else {
                REX_LOG_WARN("WorkerW",
                             "Wallpaper layer not acquired; retrying reacquire...");
            }
            if (!ReacquireWorkerW(requestSpawn)) {
                REX_LOG_ERROR("WorkerW",
                             "Reacquire failed; will retry on the next check");
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

HWND WorkerWManager::FindBlankWorkerW(bool requestWorkerWSpawn) {
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman) {
        REX_LOG_ERROR("WorkerW", "Progman not found — desktop layer unavailable");
        return nullptr;
    }

    // Force Explorer to spawn the wallpaper WorkerW. Each send runs on
    // Explorer's UI thread with a 1 s timeout, so this is only worth paying
    // when the shell may not have a WorkerW yet (first discovery / Explorer
    // restart) — never on a plain layer-invalid repair.
    if (requestWorkerWSpawn) {
        for (int i = 0; i < 4; i++) {
            SendMessageTimeoutW(progman, WM_SPAWNWORKERW, 0, 0,
                                SMTO_NORMAL, 1000, nullptr);
            Sleep(100);
        }
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
        // NOTE: the class MUST NOT be L"WorkerW". Explorer treats windows
        // of its own WorkerW class as desktop-layer peers (sends them
        // synchronous shell messages); a same-named imposter both confuses
        // discovery and opens a cross-process SendMessage deadlock that has
        // hung Explorer (AppHangXProcB1 with our exe in P6).
        REX_LOG_WARN("WorkerW", "Creating synthetic WorkerW under Progman");
        WNDCLASSW wc = {};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"REXSyntheticWorkerW";
        RegisterClassW(&wc);

        target = CreateWindowExW(
            // Input-transparent even in the failure case: if the shell ever
            // leaves this layer above SHELLDLL_DefView, clicks must still
            // reach the desktop icons instead of dying on our window. The
            // render window inside is transparent too, but hit testing falls
            // through to THIS window next — without these styles it swallowed
            // every desktop click.
            WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
            L"REXSyntheticWorkerW", L"",
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
            // paint layer exists we anchor to the sibling above it.
            // Fail-safe direction is DOWN: if there is no sibling above
            // the paint layer (it is already topmost, or identification is
            // uncertain), HWND_BOTTOM keeps us behind everything instead of
            // HWND_TOP, which would put the video above the desktop icons
            // and swallow all icon/taskbar input.
            HWND insertAfter = HWND_BOTTOM;
            HWND paintLayer = FindPaintLayerWorkerW(progman, target);
            if (paintLayer) {
                HWND abovePaint = GetWindow(paintLayer, GW_HWNDPREV);
                if (abovePaint) {
                    insertAfter = abovePaint;
                    WDLOG("Synthetic WorkerW: paint layer %p, insertAfter=%p (above paint layer)",
                          paintLayer, insertAfter);
                } else {
                    WDLOG("Synthetic WorkerW: paint layer %p is topmost; using HWND_BOTTOM (fail-safe)",
                          paintLayer);
                }
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
    // Stock black brush: CreateSolidBrush here leaked one GDI object per
    // render-window creation (never DeleteObject'd on any path).
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));

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

    // P3 / G1+G2: stash the manager for WndProc's session/power handling.
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

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
    // Instance recovery: CreateRenderWindow stashes `this` in USERDATA.
    auto* self = reinterpret_cast<WorkerWManager*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    // P3 / G1+G2: session lock and console-display state drive the
    // engine's suspend check (any source pauses, resume when all clear).
    if (self) {
        if (msg == WM_WTSSESSION_CHANGE) {
            if (wParam == WTS_SESSION_LOCK) {
                self->m_sessionLocked.store(true, std::memory_order_release);
                REX_LOG_INFO("WorkerW", "session locked — suspending wallpaper");
            } else if (wParam == WTS_SESSION_UNLOCK) {
                self->m_sessionLocked.store(false, std::memory_order_release);
                REX_LOG_INFO("WorkerW", "session unlocked — resuming wallpaper");
            }
        } else if (msg == WM_POWERBROADCAST && wParam == PBT_POWERSETTINGCHANGE) {
            const auto* setting =
                reinterpret_cast<PPOWERBROADCAST_SETTING>(lParam);
            if (setting && setting->PowerSetting == GUID_CONSOLE_DISPLAY_STATE &&
                setting->DataLength >= sizeof(DWORD)) {
                const DWORD state = *reinterpret_cast<const DWORD*>(setting->Data);
                const bool off = (state == 0);
                if (self->m_displayOff.exchange(off, std::memory_order_acq_rel) != off) {
                    REX_LOG_INFO("WorkerW", "console display {} — {} wallpaper",
                                 off ? "off" : "on",
                                 off ? "suspending" : "resuming");
                }
            }
        }
    }

    // NOTE: no PostQuitMessage here. This is a child render window, not an
    // application loop owner; posting quit from it can terminate whatever
    // thread pumps messages (observed host-loop exits). Lifetime is owned
    // by DestroyWindow from Acquire/Release/Reacquire paths.
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ─── Session/display power state (P3 / G1+G2) ────────────────────────────

void WorkerWManager::RegisterPowerNotifications() {
    if (!m_renderWindow || !IsWindow(m_renderWindow)) return;
    // Best-effort each: a dead window or denied session fails silently;
    // polling fallbacks (power status) still work without these.
    const BOOL wts = WTSRegisterSessionNotification(m_renderWindow,
                                                    NOTIFY_FOR_THIS_SESSION);
    m_powerNotify = RegisterPowerSettingNotification(
        m_renderWindow, &GUID_CONSOLE_DISPLAY_STATE,
        DEVICE_NOTIFY_WINDOW_HANDLE);
    REX_LOG_DEBUG("WorkerW", "power notifications registered (wts={}, power={})",
                  wts ? "ok" : "failed", m_powerNotify ? "ok" : "failed");
}

void WorkerWManager::UnregisterPowerNotifications() {
    if (m_powerNotify) {
        UnregisterPowerSettingNotification(m_powerNotify);
        m_powerNotify = nullptr;
    }
    if (m_renderWindow && IsWindow(m_renderWindow)) {
        WTSUnRegisterSessionNotification(m_renderWindow);
    }
}

} // namespace rex::platform::windows
