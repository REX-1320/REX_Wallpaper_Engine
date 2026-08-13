#include "MonitorEdid.hpp"

#include <windows.h>

#include <string>
#include <vector>

namespace rex::platform::windows {

uint64_t GetMonitorEdidHash(HMONITOR monitor) {
    if (!monitor) return 0;

    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(monitor, &mi)) {
        return 0; // mi.szDevice = "\\.\DISPLAY1"
    }

    // The monitor device ID maps to the PnP enumeration registry branch:
    //   HKLM\SYSTEM\CurrentControlSet\Enum\MONITOR\<id>\<instance>\Device Parameters\EDID
    DISPLAY_DEVICEW dd = {};
    dd.cb = sizeof(dd);
    if (!EnumDisplayDevicesW(mi.szDevice, 0, &dd, 0)) {
        return 0;
    }

    const std::wstring keyPath =
        L"SYSTEM\\CurrentControlSet\\Enum\\" + std::wstring(dd.DeviceID) +
        L"\\Device Parameters";

    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyPath.c_str(), 0, KEY_READ,
                      &hKey) != ERROR_SUCCESS) {
        return 0;
    }

    std::vector<BYTE> edid;
    DWORD size = 0;
    if (RegQueryValueExW(hKey, L"EDID", nullptr, nullptr, nullptr,
                         &size) == ERROR_SUCCESS && size > 0) {
        edid.resize(size);
        if (RegQueryValueExW(hKey, L"EDID", nullptr, nullptr, edid.data(),
                             &size) != ERROR_SUCCESS) {
            edid.clear();
        }
    }
    RegCloseKey(hKey);

    if (edid.empty()) {
        return 0;
    }

    // FNV-1a over the full EDID (128-byte base block + any extensions):
    // stable for a physical panel regardless of connection order.
    uint64_t hash = 1469598103934665603ull;
    for (BYTE b : edid) {
        hash ^= b;
        hash *= 1099511628211ull;
    }
    return hash;
}

} // namespace rex::platform::windows