#include "ITaskbarAdapter.hpp"
#include <windows.h>
#include <winreg.h>

namespace rex::personalization::platform {

static bool WriteRegDword(HKEY hive, const wchar_t* subKey,
                          const wchar_t* value, DWORD data) {
    HKEY key;
    if (RegCreateKeyExW(hive, subKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    LONG result = RegSetValueExW(key, value, 0, REG_DWORD,
                                 reinterpret_cast<const BYTE*>(&data), sizeof(data));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

static DWORD ReadRegDword(HKEY hive, const wchar_t* subKey,
                          const wchar_t* value, DWORD defaultVal = 0) {
    HKEY key;
    if (RegOpenKeyExW(hive, subKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return defaultVal;
    DWORD data = 0;
    DWORD size = sizeof(data);
    RegQueryValueExW(key, value, nullptr, nullptr,
                     reinterpret_cast<BYTE*>(&data), &size);
    RegCloseKey(key);
    return data;
}

TaskbarSettings WinTaskbarAdapter::read() {
    TaskbarSettings s;
    DWORD al = ReadRegDword(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
        L"TaskbarAl", 0);
    s.position = (int)al;

    DWORD ah = ReadRegDword(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
        L"TaskbarAutoHide", 0);
    s.autoHide = (ah != 0);

    DWORD small = ReadRegDword(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
        L"TaskbarSmallIcons", 0);
    s.iconSize = (small != 0) ? 0 : 1;

    return s;
}

void WinTaskbarAdapter::apply(const TaskbarSettings& s) {
    WriteRegDword(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
        L"TaskbarAl", (DWORD)s.position);

    WriteRegDword(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
        L"TaskbarAutoHide", s.autoHide ? 1 : 0);

    WriteRegDword(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
        L"TaskbarSmallIcons", (s.iconSize == 0) ? 1 : 0);

    SendNotifyMessageW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"TraySettings");
}

} // namespace
