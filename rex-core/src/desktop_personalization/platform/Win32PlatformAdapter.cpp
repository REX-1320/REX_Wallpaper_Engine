#include "Win32PlatformAdapter.hpp"
#include <windows.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <cstring>


namespace rex::desktop_personalization::platform {

// ─── Registry helpers ──────────────────────────────────────────────────────

static bool WriteRegistryDword(HKEY hive, const wchar_t* subKey,
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

static std::optional<DWORD> ReadRegistryDword(HKEY hive, const wchar_t* subKey,
                                               const wchar_t* value) {
    HKEY key;
    if (RegOpenKeyExW(hive, subKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return std::nullopt;
    DWORD data = 0;
    DWORD size = sizeof(data);
    LONG result = RegQueryValueExW(key, value, nullptr, nullptr,
                                   reinterpret_cast<BYTE*>(&data), &size);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS) return std::nullopt;
    return data;
}

// ─── Accent / theme ──────────────────────────────────────────────────────

bool Win32PlatformAdapter::setSystemDarkMode(bool dark) {
    DWORD val = dark ? 0 : 1;
    bool appOk = WriteRegistryDword(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", val);
    bool sysOk = WriteRegistryDword(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"SystemUsesLightTheme", val);
    return appOk && sysOk;
}

bool Win32PlatformAdapter::setAccentColor(const AccentColor& color) {
    DWORD accent = (static_cast<DWORD>(color.a) << 24) |
                   (static_cast<DWORD>(color.r) << 16) |
                   (static_cast<DWORD>(color.g) << 8)  |
                   static_cast<DWORD>(color.b);

    DWORD colorization = (static_cast<DWORD>(color.a) << 24) |
                         (static_cast<DWORD>(color.b) << 16) |
                         (static_cast<DWORD>(color.g) << 8)  |
                         static_cast<DWORD>(color.r);

    bool accentOk = WriteRegistryDword(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\DWM",
        L"AccentColor", accent);

    bool colorizationOk = WriteRegistryDword(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\DWM",
        L"ColorizationColor", colorization);

    bool colorBalOk = WriteRegistryDword(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\DWM",
        L"ColorizationAfterglow", colorization);

    return accentOk && colorizationOk && colorBalOk;
}

std::optional<AccentColor> Win32PlatformAdapter::getCurrentAccentColor() {
    auto val = ReadRegistryDword(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\DWM",
        L"AccentColor");
    if (!val.has_value()) return std::nullopt;

    AccentColor c;
    c.a = static_cast<uint8_t>((val.value() >> 24) & 0xFF);
    c.r = static_cast<uint8_t>((val.value() >> 16) & 0xFF);
    c.g = static_cast<uint8_t>((val.value() >> 8) & 0xFF);
    c.b = static_cast<uint8_t>(val.value() & 0xFF);
    return c;
}

// ─── Visual effects (DWM) ─────────────────────────────────────────────────

bool Win32PlatformAdapter::setTransparency(bool enable) {
    return WriteRegistryDword(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"EnableTransparency", enable ? 1 : 0);
}

bool Win32PlatformAdapter::setBlur(bool enable) {
    BOOL val = enable ? TRUE : FALSE;
    HRESULT hr = DwmSetWindowAttribute(
        nullptr, DWMWA_USE_HOSTBACKDROPBRUSH,
        &val, sizeof(val));
    return SUCCEEDED(hr);
}

bool Win32PlatformAdapter::setAcrylic(bool enable) {
    BOOL val = enable ? TRUE : FALSE;
    HRESULT hr = DwmSetWindowAttribute(
        nullptr, DWMWA_USE_HOSTBACKDROPBRUSH,
        &val, sizeof(val));
    return SUCCEEDED(hr);
}

bool Win32PlatformAdapter::setMica(bool enable) {
    DWM_SYSTEMBACKDROP_TYPE backdrop =
        enable ? DWMSBT_MAINWINDOW : DWMSBT_NONE;
    HRESULT hr = DwmSetWindowAttribute(
        nullptr, DWMWA_SYSTEMBACKDROP_TYPE,
        &backdrop, sizeof(backdrop));
    return SUCCEEDED(hr);
}

bool Win32PlatformAdapter::setAnimations(bool enable) {
    ANIMATIONINFO info = {};
    info.cbSize = sizeof(info);
    info.iMinAnimate = enable ? 1 : 0;
    return SystemParametersInfo(SPI_SETANIMATION, 0, &info, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
}

bool Win32PlatformAdapter::setAnimationSpeed(int ms) {
    DWORD val = (ms <= 0) ? 0 : static_cast<DWORD>(ms);
    return WriteRegistryDword(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
        L"AnimationsSpeed", val);
}

// ─── Desktop icons / shell ────────────────────────────────────────────────

bool Win32PlatformAdapter::setDesktopIconSize(int size) {
    int logicalSize = -size;
    return WriteRegistryDword(
        HKEY_CURRENT_USER,
        L"Control Panel\\Desktop\\WindowMetrics",
        L"Shell Icon Size", static_cast<DWORD>(logicalSize));
}

bool Win32PlatformAdapter::setDesktopIconSpacing(int horiz, int vert) {
    auto makeMetric = [](int v) -> std::wstring {
        wchar_t buf[32];
        swprintf_s(buf, L"-%d", v);
        return buf;
    };
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
            L"Control Panel\\Desktop\\WindowMetrics",
            0, nullptr, REG_OPTION_NON_VOLATILE,
            KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;

    auto hStr = makeMetric(horiz);
    auto vStr = makeMetric(vert);
    bool ok = true;
    ok = ok && (RegSetValueExW(key, L"Shell Icon Spacing", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(hStr.c_str()),
        static_cast<DWORD>((hStr.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS);
    ok = ok && (RegSetValueExW(key, L"Shell Icon Vertical Spacing", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(vStr.c_str()),
        static_cast<DWORD>((vStr.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS);
    RegCloseKey(key);
    return ok;
}

bool Win32PlatformAdapter::setDesktopLabelsVisible(bool visible) {
    DWORD val = visible ? 1 : 2;
    return WriteRegistryDword(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
        L"ShowLabel", val);
}

bool Win32PlatformAdapter::refreshDesktop() {
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE,
                        0, (LPARAM)L"Policy",
                        SMTO_ABORTIFHUNG, 2000, nullptr);
    return true;
}

// ─── Explorer / taskbar ───────────────────────────────────────────────────

bool Win32PlatformAdapter::restartExplorer() {
    HANDLE hToken;
    TOKEN_PRIVILEGES tkp;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
        return false;
    if (!LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tkp.Privileges[0].Luid))
        return false;
    tkp.PrivilegeCount = 1;
    tkp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(hToken, FALSE, &tkp, 0, nullptr, nullptr);
    CloseHandle(hToken);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    wchar_t cmd[] = L"taskkill /f /im explorer.exe";
    if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    Sleep(500);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    wchar_t run[] = L"explorer.exe";
    if (!CreateProcessW(nullptr, run, nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

bool Win32PlatformAdapter::setTaskbarAutoHide(bool autoHide) {
    return WriteRegistryDword(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StuckRects2",
        L"Settings", autoHide ? 1 : 0);
}

bool Win32PlatformAdapter::setTaskbarAlignment(int alignment) {
    DWORD val = (alignment == 0) ? 0 : 1;
    return WriteRegistryDword(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
        L"TaskbarAl", val);
}

bool Win32PlatformAdapter::applyAllChanges() {
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE,
                        0, (LPARAM)L"Policy",
                        SMTO_ABORTIFHUNG, 3000, nullptr);
    return true;
}

} // namespace rex::desktop_personalization::platform
