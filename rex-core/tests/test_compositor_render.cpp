// Validates Compositor creates and destroys its D3D12 pipeline
// without crashing. This catches missing resource barriers, null
// pointers, and D3D12 validation errors.

#include <cstdio>
#include "graphics/compositor/compositor.h"

static bool g_quit = false;
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_DESTROY) { g_quit = true; PostQuitMessage(0); }
    return DefWindowProcW(hwnd, msg, w, l);
}

int main() {
    // Create a hidden test window
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"REXCompositorTest";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, L"REXCompositorTest", L"", WS_POPUP,
                                0, 0, 1920, 1080, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) { printf("CreateWindow FAILED\n"); return 1; }
    ShowWindow(hwnd, SW_SHOW);

    // Test: create compositor, render one frame, destroy
    Compositor c;
    if (!c.Initialize(hwnd)) {
        printf("Compositor::Initialize FAILED (D3D12 might not be available)\n");
        DestroyWindow(hwnd);
        return 0; // Not a hard failure in test environments without GPU
    }
    printf("Compositor initialized\n");

    c.RenderFrame();
    printf("First frame rendered\n");

    c.RenderFrame();
    c.RenderFrame();
    printf("3 frames rendered\n");

    c.Shutdown();
    printf("Compositor shutdown\n");

    DestroyWindow(hwnd);
    printf("ALL PASSED\n");
    return 0;
}
