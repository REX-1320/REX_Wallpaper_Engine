#pragma once

#include <windows.h>
#include <string>

namespace rex::wallpaper::platform {

bool IsWindows10OrGreater();
bool IsWindows11OrGreater();
std::string GetWindowsVersion();

// ─── Window Helpers ──────────────────────────────────────────────────────

HWND CreateMessageWindow(const std::string& name);
void DestroyMessageWindow(HWND hwnd);
bool PumpMessages();

} // namespace rex::wallpaper::platform
