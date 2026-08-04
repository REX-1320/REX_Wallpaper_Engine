#include "IStartMenuAdapter.hpp"
#include <windows.h>

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

StartMenuSettings WinStartMenuAdapter::read() {
    StartMenuSettings s;
    return s;
}

void WinStartMenuAdapter::apply(const StartMenuSettings& s) {
    WriteRegDword(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
        L"Start_ShowRecommended", s.showRecommendations ? 1 : 0);

    SendNotifyMessageW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Policy");
}

} // namespace
