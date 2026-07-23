#pragma once

#include <windows.h>
#include <string>

namespace rex::platform::windows {

bool IsWindows10OrGreater();
bool IsWindows11OrGreater();
std::string GetWindowsVersion();

HWND CreateMessageWindow(const std::string& name);
void DestroyMessageWindow(HWND hwnd);
bool PumpMessages();

} // namespace rex::platform::windows
