// ─── Paiint startup stub ───────────────────────────────────────────────────
//
// The documented `windows.startupTask` form launches its executable with no
// arguments (the schema's `uap10:Parameters` attribute exists, but is not
// documented for startupTask and is used by only a couple of apps, so it is
// not something to bet the logon path on). Paiint needs `--startup-task` so the
// main process knows this is a logon launch and can honour "start minimised".
// This tiny packaged helper exists for that: with no arguments it starts
// Paiint.exe with the flag. With --status/--enable/--disable it drives the
// package's Windows.ApplicationModel.StartupTask entry, because the Run key
// Electron writes from a packaged app is virtualized and never seen by the OS.
//
// It is not used by the portable build (no package identity → "unpackaged",
// and the app falls back to app.setLoginItemSettings).

#include <windows.h>

#include <appmodel.h>
#include <shellapi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.ApplicationModel.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr wchar_t kTaskId[] = L"PaiintStartup";
constexpr wchar_t kAppExe[] = L"Paiint.exe";

std::wstring ModuleDir() {
    wchar_t path[MAX_PATH * 4] = {};
    const DWORD len = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    if (len == 0 || len >= ARRAYSIZE(path)) return {};
    std::wstring dir(path, len);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    return dir.substr(0, slash);
}

// resources\bin -> package root (two levels up).
std::wstring PackageRoot() {
    std::wstring dir = ModuleDir();
    if (dir.empty()) return {};
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    dir.resize(slash);
    const size_t slash2 = dir.find_last_of(L"\\/");
    if (slash2 == std::wstring::npos) return {};
    dir.resize(slash2);
    return dir;
}

void LogLine(const std::wstring& text) {
    wchar_t temp[MAX_PATH] = {};
    const DWORD len = GetTempPathW(ARRAYSIZE(temp), temp);
    if (len == 0 || len >= ARRAYSIZE(temp)) return;
    const std::wstring path = std::wstring(temp) + L"paiint-startup.log";
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t stamp[64] = {};
    swprintf_s(stamp, L"[%04u-%02u-%02u %02u:%02u:%02u] ", now.wYear,
               now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    const std::wstring line = std::wstring(stamp) + text + L"\r\n";
    DWORD written = 0;
    WriteFile(file, line.data(), (DWORD)(line.size() * sizeof(wchar_t)),
              &written, nullptr);
    CloseHandle(file);
}

void WriteOut(const char* text) {
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out == nullptr || out == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(out, text, (DWORD)strlen(text), &written, nullptr);
}

bool IsPackaged() {
    UINT32 length = 0;
    return GetCurrentPackageFullName(&length, nullptr) !=
           APPMODEL_ERROR_NO_PACKAGE;
}

int LaunchApp() {
    const std::wstring root = PackageRoot();
    if (root.empty()) {
        LogLine(L"launch: cannot resolve package root");
        return 1;
    }
    const std::wstring exe = root + L"\\" + kAppExe;
    std::wstring cmd = L"\"" + exe + L"\" --startup-task";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr,
                                   FALSE, 0, nullptr, root.c_str(), &si, &pi);
    if (!ok) {
        LogLine(L"launch: CreateProcess failed: " + exe + L" (" +
                std::to_wstring(GetLastError()) + L")");
        return 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    LogLine(L"launch: started " + exe);
    return 0;
}

const char* StateName(winrt::Windows::ApplicationModel::StartupTaskState state) {
    using winrt::Windows::ApplicationModel::StartupTaskState;
    switch (state) {
        case StartupTaskState::Enabled: return "enabled";
        case StartupTaskState::Disabled: return "disabled";
        case StartupTaskState::DisabledByUser: return "disabled-by-user";
        case StartupTaskState::DisabledByPolicy: return "disabled-by-policy";
        case StartupTaskState::EnabledByPolicy: return "enabled-by-policy";
        default: return "unknown";
    }
}

int StartupTaskCommand(const std::wstring& verb) {
    if (!IsPackaged()) {
        // Diagnostic for the packaged smoke test: if a child spawned from the
        // app ever lands here, it did not inherit package identity.
        LogLine(L"startup-task: unpackaged (no package identity)");
        WriteOut("unpackaged\n");
        return 2;
    }
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        const auto task =
            winrt::Windows::ApplicationModel::StartupTask::GetAsync(kTaskId)
                .get();
        if (verb == L"--status") {
            const std::string line =
                std::string(StateName(task.State())) + "\n";
            WriteOut(line.c_str());
            return 0;
        }
        if (verb == L"--enable") {
            const auto state = task.RequestEnableAsync().get();
            const std::string line =
                std::string(StateName(state)) + "\n";
            WriteOut(line.c_str());
            // Policy can enable the task even when the request was denied;
            // either state means the logon launch will happen.
            if (state == winrt::Windows::ApplicationModel::StartupTaskState::
                             Enabled ||
                state == winrt::Windows::ApplicationModel::StartupTaskState::
                             EnabledByPolicy) {
                LogLine(L"enable: enabled");
                return 0;
            }
            LogLine(L"enable: not enabled (user/policy may own the task)");
            return 1;
        }
        if (verb == L"--disable") {
            task.Disable();
            // Disable() cannot win against policy, so report what the task
            // actually is instead of claiming success unconditionally.
            const auto state = task.State();
            const std::string line =
                std::string(StateName(state)) + "\n";
            WriteOut(line.c_str());
            if (state == winrt::Windows::ApplicationModel::StartupTaskState::
                             Enabled ||
                state == winrt::Windows::ApplicationModel::StartupTaskState::
                             EnabledByPolicy) {
                LogLine(L"disable: still enabled (policy owns the task)");
                return 1;
            }
            LogLine(L"disable: ok");
            return 0;
        }
    } catch (const winrt::hresult_error& err) {
        LogLine(L"startup-task: hresult 0x" +
                std::to_wstring(static_cast<unsigned>(err.code())) + L" " +
                err.message().c_str());
        WriteOut("error\n");
        return 1;
    } catch (...) {
        LogLine(L"startup-task: unknown failure");
        WriteOut("error\n");
        return 1;
    }
    WriteOut("error\n");
    return 1;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring verb;
    if (argv && argc > 1) verb = argv[1];
    if (argv) LocalFree(argv);

    if (verb.empty()) return LaunchApp();
    return StartupTaskCommand(verb);
}
