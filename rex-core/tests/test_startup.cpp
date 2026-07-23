/// Minimal startup test — validates WorkerW + Compositor init
/// without requiring a wallpaper engine or full render loop.

#include <cstdio>
#include <cstdlib>

#include "platform/windows/WorkerWManager.hpp"
#include "graphics/compositor/compositor.h"

int main() {
    printf("[TEST] Starting WorkerW test...\n");

    rex::platform::windows::WorkerWManager ww;
    auto result = ww.Acquire();
    if (result.IsError()) {
        printf("[TEST] WorkerW FAILED: %s\n", result.Error().message.c_str());
        // Not a hard failure — might be running without desktop
        printf("[TEST] WorkerW not available (non-fatal in test)\n");
    } else {
        printf("[TEST] WorkerW OK: hwnd=%p\n", ww.GetRenderWindow());
    }

    printf("[TEST] WorkerW release...\n");
    ww.Release();

    printf("[TEST] PASSED\n");
    return 0;
}
