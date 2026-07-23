#include <windows.h>
#include <mmsystem.h>
#include <cstdio>
#include <csignal>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>

#include "wallpaper/playback/PlaybackController.hpp"
#include <shared/Result.hpp>

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
static rex::wallpaper::PlaybackController g_playback;
static bool g_isVideo = false;

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

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR lpCmdLine, int) {
    // ── DPI Awareness ──────────────────────────────────────────────────
    // Must be set before any window creation or GetSystemMetrics calls.
    // Without this, apps on high-DPI displays (e.g. 4K @ 150%) get
    // virtualized to a lower resolution, causing partial screen coverage.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

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

    // ── Load media from command line (video or image) ─────────────────
    if (lpCmdLine && lpCmdLine[0] != '\0') {
        std::string path(lpCmdLine);
        // Trim trailing whitespace
        while (!path.empty() && (path.back() == ' ' || path.back() == '\t')) {
            path.pop_back();
        }
        // Strip surrounding quotes if present
        if (path.size() >= 2 && path.front() == '\"' && path.back() == '\"') {
            path = path.substr(1, path.size() - 2);
        }

        // Detect video files by extension
        std::string ext;
        auto dot = path.rfind('.');
        if (dot != std::string::npos) {
            ext = path.substr(dot);
            std::transform(ext.begin(), ext.end(), ext.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        }

        bool isVideoExt = (ext == ".mp4" || ext == ".avi" || ext == ".mkv" || ext == ".mov" || ext == ".webm");

        if (isVideoExt) {
            g_isVideo = true;
            if (log) { fprintf(log, "Video path: '%s'\n", path.c_str()); fflush(log); }
            auto result = g_playback.Open(path);
            if (result.IsSuccess()) {
                g_playback.Play();
                if (log) { fprintf(log, "Video opened: %dx%d\n", g_playback.GetWidth(), g_playback.GetHeight()); fflush(log); }
                // Submit an empty layer for video (texture created on first frame)
                Layer bgLayer;
                bgLayer.id = 1;
                bgLayer.kind = 0;
                bgLayer.bounds[0] = 0.0f;
                bgLayer.bounds[1] = 0.0f;
                bgLayer.bounds[2] = g_compositor.GetViewportWidth();
                bgLayer.bounds[3] = g_compositor.GetViewportHeight();
                bgLayer.opacity = 1.0f;
                bgLayer.zOrder = 0;
                bgLayer.visible = true;
                g_compositor.SubmitLayer(bgLayer);
                if (log) { fprintf(log, "Video layer submitted\n"); fflush(log); }
            } else {
                if (log) { fprintf(log, "Video open FAILED: %s\n", result.Error().message.c_str()); fflush(log); }
                g_isVideo = false;
            }
        } else {
            if (log) { fprintf(log, "Image path: '%s'\n", path.c_str()); fflush(log); }
            bool ok = g_compositor.SetBackgroundFromFile(path.c_str());
            if (log) { fprintf(log, "SetBackgroundFromFile: %s\n", ok ? "OK" : "FAILED"); fflush(log); }
        }
    }

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

        // ── Video frame update ───────────────────────────────────────
        if (g_isVideo && g_playback.IsPlaying()) {
            rex::wallpaper::DecodedFrame frame = g_playback.Tick();
            if (frame.valid) {
                g_compositor.UpdateLayerPixels(1, frame.pixels.data(),
                    frame.width, frame.height, frame.rowPitch);
            }
        }

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
    g_playback.Stop();
    g_playback.Close();
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
