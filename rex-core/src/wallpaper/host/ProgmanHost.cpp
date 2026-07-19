#include "ProgmanHost.hpp"
#include <shared/Logger.hpp>

#include <chrono>
#include <string>
#include <thread>

namespace rex::wallpaper::host {

namespace {

    // ── Helper: Retrieve the class name of a window for logging ──────────
    std::wstring GetWindowClass(HWND hwnd) {
        wchar_t buffer[256];
        if (GetClassNameW(hwnd, buffer, 256)) {
            return std::wstring(buffer);
        }
        return L"(unknown)";
    }

} // anonymous namespace

// ── Construction / Destruction ─────────────────────────────────────────────

ProgmanHost::ProgmanHost() {
    m_hInstance = GetModuleHandleW(nullptr);
}

ProgmanHost::~ProgmanHost() {
    StopMonitor();
    Release();
}

// ── Acquire ─────────────────────────────────────────────────────────────────

shared::Result<void> ProgmanHost::Acquire() {
    if (m_acquired) {
        return shared::Result<void>(shared::ErrorCode::AlreadyInitialized,
            "ProgmanHost already acquired");
    }

    REX_LOG_INFO("ProgmanHost", "Acquiring Progman desktop host...");

    // ── Step 1: Find Progman ─────────────────────────────────────────────
    m_progman = FindWindowW(L"Progman", nullptr);
    if (!m_progman) {
        return shared::Result<void>(shared::ErrorCode::NotFound,
            "Progman window not found. Is explorer.exe running?");
    }

    REX_LOG_DEBUG("ProgmanHost", "Found Progman: {}", fmt::ptr(m_progman));

    // ── Step 2: Validate Progman ─────────────────────────────────────────
    if (!IsWindow(m_progman)) {
        return shared::Result<void>(shared::ErrorCode::InvalidState,
            "Progman handle is not a valid window");
    }

    if (!IsWindowVisible(m_progman)) {
        return shared::Result<void>(shared::ErrorCode::InvalidState,
            "Progman exists but is not visible — shell may be in an "
            "unusual state");
    }

    RECT progmanRect;
    if (!GetWindowRect(m_progman, &progmanRect)) {
        return shared::Result<void>(shared::ErrorCode::UnknownError,
            "Failed to get Progman window dimensions");
    }

    m_width  = GetSystemMetrics(SM_CXSCREEN);
    m_height = GetSystemMetrics(SM_CYSCREEN);

    int pW = progmanRect.right - progmanRect.left;
    int pH = progmanRect.bottom - progmanRect.top;

    REX_LOG_DEBUG("ProgmanHost", "Progman rect: {}×{}, desktop: {}×{}",
                  pW, pH, m_width, m_height);

    if (pW < m_width / 2 || pH < m_height / 2) {
        return shared::Result<void>(shared::ErrorCode::InvalidState,
            "Progman window is too small to host wallpaper content. "
            "Expected at least half the desktop dimensions.");
    }

    REX_LOG_INFO("ProgmanHost", "Progman validated: {} ({}×{})",
                 fmt::ptr(m_progman), pW, pH);

    // ── Step 3: Register window class ───────────────────────────────────
    constexpr wchar_t CLASS_NAME[] = L"REXProgmanRender";

    WNDCLASSW wc = {};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = m_hInstance;
    wc.lpszClassName = CLASS_NAME;
    // Black background — prevents white flash before D3D12 starts
    // rendering. The brush is a system GDI object; no leak.
    wc.hbrBackground = CreateSolidBrush(RGB(0, 0, 0));

    if (!RegisterClassW(&wc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            return shared::Result<void>(shared::ErrorCode::UnknownError,
                "RegisterClassW failed for Progman render window (error: " +
                std::to_string(err) + ")");
        }
    }

    // ── Step 4: Create render window as a top-level popup ──────────────
    //
    // HWND_BOTTOM alone puts us at the same Z-order position as
    // Progman (which is also at the bottom). Since we were created
    // AFTER Progman, we appear ON TOP of it — covering the icons.
    //
    // Fix: use hWndInsertAfter = Progman's HWND to place us BELOW
    // Progman in the system Z-order chain. This puts our content
    // behind Progman and its children (WorkerW → DefView → icons).

    m_renderWindow = CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
        CLASS_NAME,
        L"REX Wallpaper",
        WS_POPUP | WS_VISIBLE,
        0, 0,
        m_width, m_height,
        nullptr,   // top-level, no parent
        nullptr,
        m_hInstance,
        nullptr);

    if (!m_renderWindow) {
        DWORD err = GetLastError();
        return shared::Result<void>(shared::ErrorCode::UnknownError,
            "CreateWindowExW failed (error: " +
            std::to_string(err) + ")");
    }

    REX_LOG_DEBUG("ProgmanHost", "Render window created: {}",
                  fmt::ptr(m_renderWindow));

    // ── Step 5: Insert BELOW Progman in global Z-order ─────────────────
    //
    // SetWindowPos(hWndInsertAfter = Progman) places us immediately
    // BELOW Progman in the desktop Z-order. DWM composites top-level
    // windows bottom-to-top by Z-order, so our content appears:
    //   DWM wallpaper → our red → Progman → WorkerW/DefView → icons
    //
    // The desktop icons (children of DefView inside WorkerW/Progman)
    // render on top of Progman, which is on top of us. Icons remain
    // visible and clickable.

    SetWindowPos(m_renderWindow, m_progman,
                 0, 0, m_width, m_height,
                 SWP_NOACTIVATE | SWP_FRAMECHANGED);

    ShowWindow(m_renderWindow, SW_SHOW);
    UpdateWindow(m_renderWindow);

    REX_LOG_DEBUG("ProgmanHost",
        "Render window positioned below Progman");

    // ── Runtime window state dump ───────────────────────────────────────
    //
    // Log the complete window state so we can verify every property
    // at runtime. This is essential for diagnosing visibility bugs.

    {
        auto wcs2str = [](const wchar_t* wcs) -> std::string {
            if (!wcs) return "(null)";
            int len = WideCharToMultiByte(CP_UTF8, 0, wcs, -1,
                                          nullptr, 0, nullptr, nullptr);
            if (len <= 0) return "(empty)";
            std::string result(static_cast<size_t>(len - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wcs, -1,
                                &result[0], len, nullptr, nullptr);
            return result;
        };

        RECT clientRect;
        GetClientRect(m_renderWindow, &clientRect);
        RECT windowRect;
        GetWindowRect(m_renderWindow, &windowRect);

        HWND parent = GetParent(m_renderWindow);
        wchar_t parentClass[256] = {0};
        wchar_t renderClass[256] = {0};
        if (parent) GetClassNameW(parent, parentClass, 256);
        GetClassNameW(m_renderWindow, renderClass, 256);
        std::string parentClassStr = wcs2str(parentClass);
        std::string renderClassStr = wcs2str(renderClass);

        LONG style   = GetWindowLongW(m_renderWindow, GWL_STYLE);
        LONG exstyle = GetWindowLongW(m_renderWindow, GWL_EXSTYLE);
        BOOL visible = IsWindowVisible(m_renderWindow);

        REX_LOG_INFO("ProgmanHost",
            "┌── Render Window State ───────────────────────────────");
        REX_LOG_INFO("ProgmanHost",
            "│ HWND:          {}", fmt::ptr(m_renderWindow));
        REX_LOG_INFO("ProgmanHost",
            "│ Class:         {}", renderClassStr);
        REX_LOG_INFO("ProgmanHost",
            "│ Parent HWND:   {} (class: {})",
            fmt::ptr(parent), parentClassStr);
        REX_LOG_INFO("ProgmanHost",
            "│ Client rect:   {}×{}", clientRect.right, clientRect.bottom);
        REX_LOG_INFO("ProgmanHost",
            "│ Window rect:   {}×{} (left={} top={})",
            windowRect.right - windowRect.left,
            windowRect.bottom - windowRect.top,
            windowRect.left, windowRect.top);
        REX_LOG_INFO("ProgmanHost",
            "│ Visible:       {}", visible ? "YES" : "NO");
        REX_LOG_INFO("ProgmanHost",
            "│ Style:         0x{:08X} (WS_CHILD={})",
            static_cast<uint32_t>(style),
            (style & WS_CHILD) ? "YES" : "NO");
        REX_LOG_INFO("ProgmanHost",
            "│ ExStyle:       0x{:08X}", static_cast<uint32_t>(exstyle));
        REX_LOG_INFO("ProgmanHost",
            "│ Desktop:       {}×{}", m_width, m_height);

        // Check sibling windows in Progman
        HWND sibling = GetWindow(m_progman, GW_CHILD);
        int siblingCount = 0;
        REX_LOG_INFO("ProgmanHost",
            "│ Progman children:");
        while (sibling && siblingCount < 10) {
            wchar_t sibClass[256] = {0};
            GetClassNameW(sibling, sibClass, 256);
            std::string sibClassStr = wcs2str(sibClass);
            RECT sibRect;
            GetWindowRect(sibling, &sibRect);
            REX_LOG_INFO("ProgmanHost",
                "│   [{:d}] HWND={} class={} rect={}×{} visible={}",
                siblingCount, fmt::ptr(sibling), sibClassStr,
                sibRect.right - sibRect.left,
                sibRect.bottom - sibRect.top,
                IsWindowVisible(sibling) ? "Y" : "N");
            sibling = GetWindow(sibling, GW_HWNDNEXT);
            siblingCount++;
        }
        REX_LOG_INFO("ProgmanHost",
            "└──────────────────────────────────────────────────────");
    }

    // ── Step 6: Validate the render window ──────────────────────────────
    if (!IsWindow(m_renderWindow)) {
        Release();
        return shared::Result<void>(shared::ErrorCode::InvalidState,
            "Render window destroyed immediately after creation");
    }

    if (!IsWindowVisible(m_renderWindow)) {
        REX_LOG_WARN("ProgmanHost",
            "Render window is not visible — wallpaper may not appear");
    }

    // Validate dimensions match expectations
    RECT childRect;
    if (GetWindowRect(m_renderWindow, &childRect)) {
        int cW = childRect.right - childRect.left;
        int cH = childRect.bottom - childRect.top;

        if (cW < m_width / 2 || cH < m_height / 2) {
            Release();
            return shared::Result<void>(shared::ErrorCode::InvalidState,
                "Render window dimensions are unexpectedly small: " +
                std::to_string(cW) + "×" + std::to_string(cH));
        }
    }

    // ── Step 7: Verify z-order — keep below Progman ────────────────────
    // Re-assert our Z-order position below Progman to handle any
    // Z-order changes made by Explorer or other shell components.
    SetWindowPos(m_renderWindow, m_progman,
                 0, 0, m_width, m_height,
                 SWP_NOACTIVATE | SWP_FRAMECHANGED);

    m_acquired = true;

    REX_LOG_INFO("ProgmanHost",
        "Progman desktop host acquired: {}×{} window at bottom of "
        "Progman z-order (Progman={}, render={})",
        m_width, m_height,
        fmt::ptr(m_progman), fmt::ptr(m_renderWindow));

    return shared::Result<void>();
}

// ── Release ─────────────────────────────────────────────────────────────────

void ProgmanHost::Release() {
    if (!m_acquired) return;

    REX_LOG_DEBUG("ProgmanHost", "Releasing Progman desktop host...");

    // Destroy the render window first (removes it from Progman's tree).
    if (m_renderWindow && IsWindow(m_renderWindow)) {
        DestroyWindow(m_renderWindow);
    }
    m_renderWindow = nullptr;
    m_progman      = nullptr;
    m_acquired     = false;

    REX_LOG_INFO("ProgmanHost", "Progman desktop host released");
}

// ── Accessors ───────────────────────────────────────────────────────────────

HWND ProgmanHost::GetRenderWindow() const { return m_renderWindow; }
int  ProgmanHost::GetWidth()  const { return m_width; }
int  ProgmanHost::GetHeight() const { return m_height; }

bool ProgmanHost::IsValid() const {
    return m_acquired
        && m_renderWindow != nullptr
        && IsWindow(m_renderWindow);
}

bool ProgmanHost::IsAcquired() const { return m_acquired; }

std::string_view ProgmanHost::GetHostType() const { return "Progman"; }

// ── Window Procedure ────────────────────────────────────────────────────────

LRESULT CALLBACK ProgmanHost::WndProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    case WM_ERASEBKGND:
        // The window background is black (set by RegisterClass).
        // Returning 1 prevents the DefWindowProc from further
        // erasing and avoids flicker during D3D12 rendering.
        return 1;

    case WM_PAINT: {
        // We render exclusively through D3D12. Validate the region
        // to prevent the ack-nowledgement from generating infinite
        // WM_PAINT messages, but do not issue any GDI drawing.
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

// ── Shell Monitoring ───────────────────────────────────────────────────────

void ProgmanHost::StartMonitor(std::function<void()> onRecovery) {
    if (m_monitoring) return;

    m_recoveryCallback = std::move(onRecovery);
    m_monitoring.store(true, std::memory_order_release);
    m_monitorThread = std::thread(&ProgmanHost::MonitorShell, this);

    REX_LOG_INFO("ProgmanHost", "Shell monitor started");
}

void ProgmanHost::StopMonitor() {
    if (!m_monitoring.load()) return;

    m_monitoring.store(false, std::memory_order_release);
    if (m_monitorThread.joinable()) {
        m_monitorThread.join();
    }

    REX_LOG_INFO("ProgmanHost", "Shell monitor stopped");
}

void ProgmanHost::MonitorShell() {
    using namespace std::chrono_literals;

    REX_LOG_DEBUG("ProgmanHost", "Monitor thread entered");

    while (m_monitoring.load(std::memory_order_acquire)) {
        // Check whether Progman still exists and is the same window.
        HWND currentProgman = FindWindowW(L"Progman", nullptr);
        bool progmanChanged = (currentProgman != m_progman);

        // Check whether our child window is still alive.
        bool windowAlive = (m_renderWindow != nullptr)
                        && IsWindow(m_renderWindow);

        if (!progmanChanged && windowAlive) {
            // Everything is healthy — sleep and retry.
            std::this_thread::sleep_for(2000ms);
            continue;
        }

        // ── Something went wrong — attempt recovery ────────────────────
        if (progmanChanged && currentProgman) {
            REX_LOG_INFO("ProgmanHost",
                "Progman changed (was {}, now {}). "
                "Shell likely restarted. Reacquiring...",
                fmt::ptr(m_progman), fmt::ptr(currentProgman));
        } else if (!windowAlive && m_progman && IsWindow(m_progman)) {
            REX_LOG_WARN("ProgmanHost",
                "Render window destroyed but Progman still alive. "
                "Reacquiring render window...");
        } else if (!currentProgman) {
            REX_LOG_WARN("ProgmanHost",
                "Progman not found. Waiting for shell to start...");
            std::this_thread::sleep_for(500ms);
            continue;
        }

        // Release all old state.
        // Do NOT call the public Release() because we are on the
        // monitor thread and must not race with the render thread
        // trying to present to the old window. Instead, just clean
        // up our handles.
        HWND oldRender = m_renderWindow;
        m_renderWindow = nullptr;
        m_progman      = nullptr;
        m_acquired     = false;

        if (oldRender && IsWindow(oldRender)) {
            DestroyWindow(oldRender);
        }

        // Brief delay to let the shell stabilize after restart.
        std::this_thread::sleep_for(1000ms);

        // Re-acquire on the new Progman.
        auto result = const_cast<ProgmanHost*>(this)->Acquire();
        if (result.IsSuccess()) {
            REX_LOG_INFO("ProgmanHost",
                "Reacquired after shell restart (type={})",
                GetHostType());
            if (m_recoveryCallback) {
                m_recoveryCallback();
            }
        } else {
            REX_LOG_ERROR("ProgmanHost",
                "Failed to reacquire after shell restart: {}",
                result.Error().message);
        }

        // Wait before the next monitor cycle.
        std::this_thread::sleep_for(2000ms);
    }

    REX_LOG_DEBUG("ProgmanHost", "Monitor thread exiting");
}

} // namespace rex::wallpaper::host
