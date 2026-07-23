#include <windows.h>
#include <mmsystem.h>
#include <cstdio>
#include <csignal>
#include <cstring>
#include <string>
#include <vector>

#include "platform/windows/DesktopHostFactory.hpp"
#include "ipc/ipc_server.h"
#include "graphics/compositor/compositor.h"
#include "graphics/animation/animation_engine.h"
#include "core/scheduler/render_scheduler.h"

static std::unique_ptr<rex::platform::windows::IDesktopHost> g_desktopHost;
static IpcServer g_ipcServer;
static Compositor g_compositor;
static AnimationEngine g_animationEngine;
static RenderScheduler g_renderScheduler;

static bool g_running = true;

void SignalHandler(int) { g_running = false; }

void OnIpcMessage(const std::vector<uint8_t>& raw) {
    if (raw.size() < 9) return;

    uint32_t msgType = *reinterpret_cast<const uint32_t*>(raw.data() + 1);

    auto readU64 = [&](size_t offset) -> uint64_t {
        uint64_t v;
        memcpy(&v, raw.data() + offset, sizeof(v));
        return v;
    };
    auto readI32 = [&](size_t offset) -> int32_t {
        int32_t v;
        memcpy(&v, raw.data() + offset, sizeof(v));
        return v;
    };

    switch (msgType) {
    case 1: {
        std::vector<uint8_t> pong = { 0, 2, 0, 0, 0, 0 };
        g_ipcServer.Send(pong);
        break;
    }
    case 4: {
        HWND hwnd = g_desktopHost ? g_desktopHost->GetRenderWindow() : nullptr;
        RECT rect;
        if (hwnd) {
            GetClientRect(hwnd, &rect);
        } else {
            rect.left = rect.top = 0;
            rect.right = rect.bottom = 0;
        }
        std::string status = "{\"workerw_attached\":true,\"uptime_ms\":0}";
        std::vector<uint8_t> response(status.begin(), status.end());
        g_ipcServer.Send(response);
        break;
    }
    case 6: {
        printf("[REX] Shutdown requested by shell\n");
        g_running = false;
        break;
    }
    case 99: {
        if (raw.size() >= 19) {
            float r = *reinterpret_cast<const float*>(raw.data() + 9);
            float g = *reinterpret_cast<const float*>(raw.data() + 13);
            float b = *reinterpret_cast<const float*>(raw.data() + 17);
            g_compositor.SetClearColor(r, g, b);
            printf("[REX] Triangle color set to (%.2f, %.2f, %.2f)\n", r, g, b);
        }
        break;
    }
    default:
        break;
    }
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    // ── DPI Awareness ──────────────────────────────────────────────────
    // Must be set before any window creation or GetSystemMetrics calls.
    // Without this, apps on high-DPI displays (e.g. 4K @ 150%) get
    // virtualized to a lower resolution, causing partial screen coverage.
    SetProcessDPIAware();

    // Debug log — write immediately in case of early crash
    FILE* log = nullptr;
    fopen_s(&log, "D:\\REX_doc\\rex-core\\build\\Release\\rex_core_startup.log", "w");
    if (log) { fprintf(log, "WinMain entered\n"); fflush(log); }

    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        AllocConsole();
    }
    FILE* fp;
    freopen_s(&fp, "CONOUT$", "w", stdout);

    timeBeginPeriod(1);
    printf("[REX] Starting REX Core v0.1.0\n");
    if (log) { fprintf(log, "Console allocated\n"); fflush(log); }

    // ── Acquire desktop host (WorkerW → Progman → error) ───────────────
    g_desktopHost = rex::platform::windows::DesktopHostFactory::CreateBestAvailable();
    if (!g_desktopHost) {
        printf("[REX] FATAL: No desktop host available. Check logs.\n");
        if (log) { fprintf(log, "FATAL: No desktop host\n"); fclose(log); }
        return 1;
    }
    if (log) { fprintf(log, "Desktop host OK (%s)\n", g_desktopHost->GetHostType().data()); fflush(log); }

    // ── Start shell monitoring ─────────────────────────────────────────
    g_desktopHost->StartMonitor([]() {
        printf("[REX] Shell restarted, re-initializing compositor...\n");
        g_compositor.Shutdown();
        g_compositor.Initialize(g_desktopHost->GetRenderWindow());
    });
    if (log) { fprintf(log, "Monitor started\n"); fflush(log); }

    // ── Initialize Compositor ──────────────────────────────────────────
    HWND targetHwnd = g_desktopHost->GetRenderWindow();
    if (log) {
        RECT cr; GetClientRect(targetHwnd, &cr);
        RECT wr; GetWindowRect(targetHwnd, &wr);
        HWND parent = GetParent(targetHwnd);
        wchar_t pcls[256] = {0};
        if (parent) GetClassNameW(parent, pcls, 256);
        fprintf(log, "Render window: HWND=%p client=%dx%d window=%dx%d parent=%p(%S) visible=%d\n",
                targetHwnd, cr.right, cr.bottom,
                wr.right - wr.left, wr.bottom - wr.top,
                parent, pcls, IsWindowVisible(targetHwnd));
        fflush(log);
    }
    printf("[REX] Compositor target: HWND=%p (%s) %dx%d\n",
           targetHwnd, g_desktopHost->GetHostType().data(),
           g_desktopHost->GetWidth(), g_desktopHost->GetHeight());

    // Force the render window to full desktop size before compositor init.
    // On Raised Desktop (Win11 24H2), a child window's client rect may
    // not reflect the desktop size until explicitly positioned.
    SetWindowPos(targetHwnd, HWND_BOTTOM,
                 0, 0,
                 g_desktopHost->GetWidth(),
                 g_desktopHost->GetHeight(),
                 SWP_NOACTIVATE | SWP_FRAMECHANGED);
    ShowWindow(targetHwnd, SW_SHOW);
    UpdateWindow(targetHwnd);
    Sleep(50);

    if (!g_compositor.Initialize(targetHwnd)) {
        printf("[REX] FATAL: Compositor initialization failed\n");
        if (log) { fprintf(log, "Compositor FAILED\n"); fclose(log); }
        return 1;
    }
    if (log) { fprintf(log, "Compositor OK\n"); fflush(log); }
    printf("[REX] Compositor initialized. Clear color: (0.00, 0.50, 0.80)\n");

    // ── Start IPC server ───────────────────────────────────────────────
    if (!g_ipcServer.Start("REX/core/v1")) {
        printf("[REX] FATAL: IPC server failed to start\n");
        if (log) { fprintf(log, "IPC FAILED\n"); fclose(log); }
        return 1;
    }
    g_ipcServer.SetMessageHandler(OnIpcMessage);
    if (log) { fprintf(log, "IPC OK\n"); fflush(log); }

    HANDLE readyEvent = CreateEventA(nullptr, FALSE, FALSE, "REX_Core_Ready");
    if (readyEvent) {
        SetEvent(readyEvent);
        CloseHandle(readyEvent);
    }

    printf("[REX] Ready. Waiting for shell connection...\n");

    // ── Main Loop ──────────────────────────────────────────────────────
    if (log) { fprintf(log, "Entering main loop\n"); fflush(log); }

    LARGE_INTEGER freq, last;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);

    int loopCount = 0;
    while (g_running) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double frameTimeMs = static_cast<double>(now.QuadPart - last.QuadPart)
                           * 1000.0 / freq.QuadPart;
        last = now;

        g_animationEngine.Tick(frameTimeMs);
        g_compositor.RenderFrame();

        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (frameTimeMs < 16.6) {
            Sleep(static_cast<DWORD>(16.6 - frameTimeMs));
        }

        if (loopCount == 0 && log) { fprintf(log, "First frame complete\n"); fflush(log); }
        loopCount++;

        // Periodic health check
        if (loopCount % 600 == 0 && log) {
            fprintf(log, "Heartbeat: %d frames\n", loopCount);
            fflush(log);
        }
    }

    printf("[REX] Shutting down...\n");
    if (log) { fprintf(log, "Shutdown. Total frames: %d\n", loopCount); fflush(log); }
    g_ipcServer.Stop();
    g_compositor.Shutdown();
    if (g_desktopHost) {
        g_desktopHost->StopMonitor();
        g_desktopHost->Release();
    }
    timeEndPeriod(1);
    printf("[REX] Shutdown complete.\n");
    return 0;
}
