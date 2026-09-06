// rex-minidump — crash-report helper: captures a full user-mode minidump of
// a live process (MiniDumpWithFullMemory | MiniDumpWithHandleData) so the
// app's crash reporter can attach native crash state to its reports.
//
// Usage: rex-minidump <pid> <dumpPath>
//   Exit codes: 0 = ok, 1 = bad args, 2 = OpenProcess failed,
//               3 = CreateFile failed, 4 = MiniDumpWriteDump failed.
//   Prints one JSON line to stdout: {"ok":true,"path":"...","pid":N,"name":"..."}
//   or {"ok":false,"error":"..."}.

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <string>

namespace {

std::string WideToUtf8(const wchar_t* w) {
    if (!w) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string out(len > 0 ? static_cast<size_t>(len - 1) : 0, '\0');
    if (len > 0) {
        WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), len, nullptr, nullptr);
    }
    return out;
}

// The helper's stdout is parsed as JSON — escape backslashes and quotes so
// Windows paths survive (e.g. C:\Users\... -> C:\\Users\\...).
std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (const char c : s) {
        if (c == '\\' || c == '"') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: rex-minidump <pid> <dumpPath>\n");
        return 1;
    }

    const DWORD pid = static_cast<DWORD>(_wtoi(argv[1]));
    const std::wstring dumpPath = argv[2];
    if (pid == 0 || dumpPath.empty()) {
        std::fprintf(stderr, "usage: rex-minidump <pid> <dumpPath>\n");
        return 1;
    }

    HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!proc) {
        std::printf("{\"ok\":false,\"error\":\"OpenProcess failed (%lu)\"}\n", GetLastError());
        return 2;
    }

    std::wstring procName;
    {
        wchar_t nameBuf[MAX_PATH];
        DWORD nameLen = MAX_PATH;
        if (QueryFullProcessImageNameW(proc, 0, nameBuf, &nameLen)) {
            procName = nameBuf;
        }
    }

    HANDLE file = CreateFileW(dumpPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        std::printf("{\"ok\":false,\"error\":\"CreateFile failed (%lu)\"}\n", GetLastError());
        CloseHandle(proc);
        return 3;
    }

    const DWORD flags = MiniDumpWithFullMemory | MiniDumpWithHandleData;
    const BOOL ok = MiniDumpWriteDump(proc, pid, file,
                                      static_cast<MINIDUMP_TYPE>(flags), nullptr, nullptr, nullptr);

    CloseHandle(file);
    CloseHandle(proc);

    if (!ok) {
        std::printf("{\"ok\":false,\"error\":\"MiniDumpWriteDump failed (%lu)\"}\n", GetLastError());
        return 4;
    }

    std::printf("{\"ok\":true,\"path\":\"%s\",\"pid\":%lu,\"name\":\"%s\"}\n",
                JsonEscape(WideToUtf8(dumpPath.c_str())).c_str(), pid,
                JsonEscape(WideToUtf8(procName.c_str())).c_str());
    return 0;
}