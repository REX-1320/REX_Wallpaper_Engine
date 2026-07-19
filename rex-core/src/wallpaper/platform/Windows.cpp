#include "Windows.hpp"
#include <shared/Logger.hpp>

namespace rex::wallpaper::platform {

bool IsWindows10OrGreater() {
    return true; // Placeholder
}

bool IsWindows11OrGreater() {
    return true; // Placeholder
}

std::string GetWindowsVersion() {
    return "Windows 11"; // Placeholder
}

HWND CreateMessageWindow(const std::string& name) {
    // Minimal window for message pump
    // Phase 0: just a placeholder
    return nullptr;
}

void DestroyMessageWindow(HWND hwnd) {
    // Placeholder
}

bool PumpMessages() {
    MSG msg;
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
        if (msg.message == WM_QUIT) {
            return false;
        }
    }
    return true;
}

} // namespace rex::wallpaper::platform
