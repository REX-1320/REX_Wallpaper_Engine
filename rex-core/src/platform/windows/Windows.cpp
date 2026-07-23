#include "Windows.hpp"
#include <shared/Logger.hpp>

namespace rex::platform::windows {

bool IsWindows10OrGreater() {
    return true;
}

bool IsWindows11OrGreater() {
    return true;
}

std::string GetWindowsVersion() {
    return "Windows 11";
}

HWND CreateMessageWindow(const std::string& name) {
    return nullptr;
}

void DestroyMessageWindow(HWND hwnd) {
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

} // namespace rex::platform::windows
