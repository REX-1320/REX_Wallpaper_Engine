#include <windows.h>
#include <mmsystem.h>
#include <cstdio>
#include <csignal>
#include <cstring>
#include <string>
#include <vector>

#include "workerw.h"
#include "ipc_server.h"
#include "compositor.h"
#include "animation_engine.h"
#include "render_scheduler.h"

static WorkerWHost g_workerW;
static IpcServer g_ipcServer;
static Compositor g_compositor;
static AnimationEngine g_animationEngine;
static RenderScheduler g_renderScheduler;

static bool g_running = true;

void SignalHandler(int) { g_running = false; }

void OnIpcMessage(const std::vector<uint8_t>& raw) {
    if (raw.size() < 9) return;

    uint32_t msgType = *reinterpret_cast<const uint32_t*>(raw.data() + 1);

    // Shared memory handle size Windows (64-bit: 8 bytes)
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
        HWND hwnd = g_workerW.GetRenderTargetWindow();
        RECT rect;
        GetClientRect(hwnd, &rect);
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
    case 10: {
        if (raw.size() >= 33) {
            uint64_t layerId = readU64(5);
            int32_t x = readI32(13);
            int32_t y = readI32(17);
            int32_t w = readI32(21);
            int32_t h = readI32(25);
            int32_t z = readI32(29);
            Layer layer = {};
            layer.id = layerId;
            layer.kind = 0;
            layer.bounds[0] = static_cast<float>(x);
            layer.bounds[1] = static_cast<float>(y);
            layer.bounds[2] = static_cast<float>(w);
            layer.bounds[3] = static_cast<float>(h);
            layer.opacity = 1.0f;
            layer.zOrder = z;
            layer.visible = true;
            g_compositor.SubmitLayer(layer);
            printf("[REX] SetWidgetTransform: layer %llu -> (%d,%d %dx%d) z=%d\n",
                layerId, x, y, w, h, z);
        }
        break;
    }
    case 11: {
        if (raw.size() >= 29) {
            uint64_t layerId = readU64(5);
            uint64_t handleVal = readU64(13);
            int32_t w = readI32(21);
            int32_t h = readI32(25);
            HANDLE sharedHandle = reinterpret_cast<HANDLE>(handleVal);
            g_compositor.ImportLayerTexture(layerId, sharedHandle, w, h);
        }
        break;
    }
    case 12: {
        if (raw.size() >= 13) {
            uint64_t layerId = readU64(5);
            g_compositor.RemoveLayerTexture(layerId);
            printf("[REX] UnregisterWidgetSurface: layer %llu\n", layerId);
        }
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
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        AllocConsole();
    }
    FILE* fp;
    freopen_s(&fp, "CONOUT$", "w", stdout);

    timeBeginPeriod(1); // 1ms timer resolution for accurate frame pacing
    printf("[REX] Starting REX Core v0.1.0\n");

    if (!g_workerW.Initialize()) {
        printf("[REX] FATAL: WorkerW initialization failed\n");
        return 1;
    }

    g_workerW.MonitorExplorer();
    g_workerW.SetOnExplorerRestarted([]() {
        printf("[REX] Explorer restarted, re-initializing compositor...\n");
        g_compositor.Shutdown();
        g_compositor.Initialize(g_workerW.GetRenderTargetWindow());
    });

    if (!g_compositor.Initialize(g_workerW.GetRenderTargetWindow())) {
        printf("[REX] FATAL: Compositor initialization failed\n");
        return 1;
    }

    if (!g_ipcServer.Start("REX/core/v1")) {
        printf("[REX] FATAL: IPC server failed to start\n");
        return 1;
    }
    g_ipcServer.SetMessageHandler(OnIpcMessage);

    HANDLE readyEvent = CreateEventA(nullptr, FALSE, FALSE, "REX_Core_Ready");
    if (readyEvent) {
        SetEvent(readyEvent);
        CloseHandle(readyEvent);
    }

    printf("[REX] Ready. Waiting for shell connection...\n");

    LARGE_INTEGER freq, last;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);

    while (g_running) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double frameTimeMs = static_cast<double>(now.QuadPart - last.QuadPart) * 1000.0 / freq.QuadPart;
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
    }

    printf("[REX] Shutting down...\n");
    g_ipcServer.Stop();
    g_compositor.Shutdown();
    g_workerW.Shutdown();
    timeEndPeriod(1);
    printf("[REX] Shutdown complete.\n");
    return 0;
}
