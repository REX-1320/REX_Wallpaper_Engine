#include <windows.h>

#include <rex/wallpaper/IWallpaperEngine.hpp>
#include <rex/wallpaper/IRendererAdapter.hpp>
#include <shared/Logger.hpp>
#include <shared/Result.hpp>

#include "wallpaper/runtime/Json.hpp"
#include "wallpaper/runtime/ClientSurface.hpp"
#include "wallpaper/runtime/FrameProtocol.hpp"
#include "wallpaper/runtime/RuntimeGuard.hpp"
#include "wallpaper/runtime/SafePath.hpp"
#include <cstdlib>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

#include <psapi.h>

#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

// Benchmark mode: FFmpeg D3D11VA decode -> D3D12 zero-copy interop.
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d11_2.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
}

#include "graphics/renderer/CommandQueue.hpp"
#include "graphics/renderer/Device.hpp"
#include "graphics/renderer/SwapChain.hpp"

#include "modules/EventBus.hpp"
#include "modules/ConfigurationStore.hpp"
#include "modules/Settings/interfaces/ISettingsService.hpp"
#include "modules/Settings/registry/SettingsRegistry.hpp"
#include "modules/Settings/services/SettingsService.hpp"
#include "modules/Settings/validation/ValidationEngine.hpp"
#include "modules/Settings/search/SettingsSearch.hpp"
#include "modules/Settings/history/SettingsHistory.hpp"
#include "modules/Settings/backup/BackupManager.hpp"
#include "modules/Settings/schema/SchemaManager.hpp"
#include "modules/Settings/io/ImportExportManager.hpp"

using namespace rex::wallpaper;

namespace json = rex::runtime;

static std::string WidenToUtf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(size - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, utf8.data(), size, nullptr, nullptr);
    return utf8;
}

static std::wstring Utf8ToWiden(const std::string& utf8) {
    if (utf8.empty()) return {};
    if (utf8.size() > 32767u * 4) return {};  // cannot widen to a valid path
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                   utf8.c_str(), -1, nullptr, 0);
    // P2-17: invalid sequences (or empty result) fail loudly instead of
    // substituting U+FFFD — and size <= 0 must never reach the allocation
    // below (size - 1 would wrap to a gigantic wstring → terminate).
    if (size <= 1) return {};
    std::wstring wide(static_cast<size_t>(size - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                        utf8.c_str(), -1, wide.data(), size);
    return wide;
}

// ─── Settings bundle (standalone, no module container) ─────────────────────
// The runtime has no ModuleOrchestrator; construct the settings service the
// same way SettingsModule::initialize does. The bundle owns the EventBus and
// ConfigurationStore the service references, so it must outlive the service.
struct SettingsBundle {
    std::shared_ptr<rex::modules::EventBus> bus;
    std::shared_ptr<rex::modules::ConfigurationStore> store;
    std::shared_ptr<rex::settings::SettingsService> service;
};

static std::shared_ptr<rex::settings::ISettingsService> CreateSettingsService() {
    static std::shared_ptr<SettingsBundle> bundle; // kept alive for the process

    if (!bundle) {
        auto b = std::make_shared<SettingsBundle>();
        b->bus = std::make_shared<rex::modules::EventBus>();
        b->store = std::make_shared<rex::modules::ConfigurationStore>(
            "config/settings.json");

        auto registry = std::make_unique<rex::settings::SettingsRegistry>();
        auto validator = std::make_unique<rex::settings::ValidationEngine>();
        auto search = std::make_unique<rex::settings::SettingsSearch>(*registry);
        auto history = std::make_unique<rex::settings::SettingsHistory>();
        auto backup = std::make_unique<rex::settings::BackupManager>(b->store);
        auto schema = std::make_unique<rex::settings::SchemaManager>();
        auto io = std::make_unique<rex::settings::ImportExportManager>(*registry, *validator);

        b->service = std::make_shared<rex::settings::SettingsService>(
            std::move(registry), std::move(validator), std::move(search),
            std::move(history), std::move(backup), std::move(schema),
            std::move(io), *b->bus, *b->store);

        // Load persisted values (e.g. per-monitor wallpaper assignments).
        b->service->hydrateSettings();
        bundle = b;
    }
    return bundle->service;
}

// Process private (committed) memory in MB â€” for the frontend telemetry.
static double GetProcessPrivateMemoryMB() {
    PROCESS_MEMORY_COUNTERS_EX pmc = {};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PPROCESS_MEMORY_COUNTERS>(&pmc), sizeof(pmc))) {
        return static_cast<double>(pmc.PrivateUsage) / (1024.0 * 1024.0);
    }
    return 0.0;
}

// Logs land in %TEMP%\REX\ so packaged builds are diagnosable regardless
// of the working directory the process was launched from.
static std::string BuildLogPath(const wchar_t* fileName) {
    wchar_t tempDir[MAX_PATH] = {};
    if (!GetTempPathW(MAX_PATH, tempDir)) {
        wcscpy_s(tempDir, L".\\");
    }
    std::wstring dir = std::wstring(tempDir) + L"REX";
    CreateDirectoryW(dir.c_str(), nullptr);
    return WidenToUtf8(dir + L"\\" + fileName);
}

// ─── Crash / hang diagnostics ─────────────────────────────────────────────
// Three diagnostic layers:
//   1. AddVectoredExceptionHandler (VEH) — fires on every first-chance
//      exception (caught or not). Useful for AV (0xC0000005). Registered
//      in wmain. Returns EXCEPTION_CONTINUE_SEARCH; benign caught exceptions
//      (e.g. D3D11-on-D3D12 init probe) are logged but not fatal.
//   2. SetUnhandledExceptionFilter — fires only on truly unhandled crashes
//      after all SEH/VEH handlers declined. Dumps all threads.
//   3. HangWatchdog — background thread that dumps all thread stacks at
//      T+10s after INIT and every 5s thereafter. Writes directly via fprintf
//      (never spdlog) so it works even when the spdlog async queue is frozen.
//
// DumpStack accepts an explicit thread HANDLE + CONTEXT so it works both from
// the VEH (faulting thread) and from the watchdog (remote suspended thread).
// SymInitialize is called once and shared; all dbghelp calls are serialized
// under g_symMutex.

#include <tlhelp32.h>

static std::mutex g_symMutex;      // serializes all dbghelp calls
static bool       g_symReady = false;
static HANDLE     g_symProc  = nullptr;

static void EnsureSymInit() {
    // Must be called under g_symMutex.
    if (g_symReady) return;
    g_symProc = GetCurrentProcess();
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring exeDir(exePath);
    const size_t sep = exeDir.find_last_of(L"\\/ ");
    if (sep != std::wstring::npos) exeDir = exeDir.substr(0, sep);
    const std::string symPath = WidenToUtf8(exeDir);
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    g_symReady = (SymInitialize(g_symProc, symPath.c_str(), TRUE) != 0);
    if (!g_symReady) {
        g_symReady = (SymInitialize(g_symProc, nullptr, TRUE) != 0);
    }
}

// Walk the stack of `thread` using the captured `ctx`.
// `thread` must already be suspended (or be GetCurrentThread()).
// Caller MUST hold g_symMutex.
static void DumpStack(FILE* f, HANDLE thread, CONTEXT ctx) {
    if (!g_symReady) {
        fprintf(f, "    (dbghelp not initialized)\n");
        return;
    }

    STACKFRAME64 sf = {};
    sf.AddrPC.Offset    = ctx.Rip;  sf.AddrPC.Mode    = AddrModeFlat;
    sf.AddrFrame.Offset = ctx.Rbp;  sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Offset = ctx.Rsp;  sf.AddrStack.Mode = AddrModeFlat;

    char symBuf[sizeof(SYMBOL_INFO) + 256] = {};
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen   = 256;

    for (int i = 0; i < 64; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, g_symProc, thread, &sf, &ctx,
                         nullptr, SymFunctionTableAccess64, SymGetModuleBase64,
                         nullptr)) break;
        if (sf.AddrPC.Offset == 0) break;

        char name[512] = "<unknown>";
        DWORD64 disp = 0;
        if (SymFromAddr(g_symProc, sf.AddrPC.Offset, &disp, sym)) {
            std::snprintf(name, sizeof(name), "%s+0x%llx", sym->Name,
                          static_cast<unsigned long long>(disp));
        }
        IMAGEHLP_LINE64 line = {};  line.SizeOfStruct = sizeof(line);
        DWORD lineDisp = 0;
        if (SymGetLineFromAddr64(g_symProc, sf.AddrPC.Offset, &lineDisp, &line)) {
            fprintf(f, "    %s [%s:%lu]\n", name, line.FileName, line.LineNumber);
        } else {
            fprintf(f, "    %s (0x%p)\n", name, reinterpret_cast<void*>(sf.AddrPC.Offset));
        }
        fflush(f);
    }
}

// Dumps every thread in this process to `f`. Uses Toolhelp32 to enumerate
// TIDs, suspends each non-self thread, captures its context, walks its stack,
// then resumes it. Acquires g_symMutex for the entire operation.
static void DumpAllThreadStacks(FILE* f) {
    const DWORD selfTid = GetCurrentThreadId();
    const DWORD pid     = GetCurrentProcessId();

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        fprintf(f, "  (CreateToolhelp32Snapshot failed: %lu)\n", GetLastError());
        return;
    }

    std::lock_guard<std::mutex> symLock(g_symMutex);
    EnsureSymInit();

    THREADENTRY32 te = {};  te.dwSize = sizeof(te);
    if (!Thread32First(snap, &te)) { CloseHandle(snap); return; }

    do {
        if (te.th32OwnerProcessID != pid) continue;
        const DWORD tid = te.th32ThreadID;

        if (tid == selfTid) {
            CONTEXT ctx = {};  ctx.ContextFlags = CONTEXT_FULL;
            RtlCaptureContext(&ctx);
            fprintf(f, "\n  [TID %lu] (watchdog / dumper thread)\n", tid);
            DumpStack(f, GetCurrentThread(), ctx);
            continue;
        }

        HANDLE th = OpenThread(
            THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
            FALSE, tid);
        if (!th) {
            fprintf(f, "\n  [TID %lu] (cannot open: %lu)\n", tid, GetLastError());
            continue;
        }

        SuspendThread(th);
        CONTEXT ctx = {};  ctx.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(th, &ctx)) {
            fprintf(f, "\n  [TID %lu]\n", tid);
            DumpStack(f, th, ctx);
        } else {
            fprintf(f, "\n  [TID %lu] (GetThreadContext failed: %lu)\n",
                    tid, GetLastError());
        }
        ResumeThread(th);
        CloseHandle(th);

    } while (Thread32Next(snap, &te));

    CloseHandle(snap);
}

static LONG WINAPI CrashHandler(PEXCEPTION_POINTERS ep) {
    // Noise filters: these fire on every benign C++ throw / debugger notice
    // and would spam crash.log; genuinely unhandled ones still reach the
    // last-chance UnhandledCrashFilter below.
    switch (ep->ExceptionRecord->ExceptionCode) {
        case 0xE06D7363:     // MSVC C++ exception (normally caught)
        case 0x40010006:     // DBG_PRINTEXCEPTION_C (OutputDebugStringA)
        case 0x4001000A:     // DBG_PRINTEXCEPTION_WIDE_C (OutputDebugStringW)
            return EXCEPTION_CONTINUE_SEARCH;
        default:
            break;
    }

    const std::string path = BuildLogPath(L"crash.log");
    FILE* f = nullptr;
    fopen_s(&f, path.c_str(), "a");  // append: preserve any watchdog dumps
    if (f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "\n=== REX VEH exception %04d-%02d-%02d %02d:%02d:%02d ===\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        fprintf(f, "Exception 0x%08lX at 0x%p (thread %lu)\n",
                static_cast<unsigned long>(ep->ExceptionRecord->ExceptionCode),
                ep->ExceptionRecord->ExceptionAddress,
                GetCurrentThreadId());
        fprintf(f, "Stack (faulting thread):\n");
        {
            std::lock_guard<std::mutex> symLock(g_symMutex);
            EnsureSymInit();
            CONTEXT ctx = *ep->ContextRecord;
            DumpStack(f, GetCurrentThread(), ctx);
        }
        fclose(f);
    }
    return EXCEPTION_CONTINUE_SEARCH;  // keep default crash behaviour
}

static LONG WINAPI UnhandledCrashFilter(PEXCEPTION_POINTERS ep) {
    const std::string path = BuildLogPath(L"crash.log");
    FILE* f = nullptr;
    fopen_s(&f, path.c_str(), "a");
    if (f) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "\n=== REX UNHANDLED CRASH %04d-%02d-%02d %02d:%02d:%02d ===\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        fprintf(f, "Exception 0x%08lX at 0x%p (thread %lu)\n",
                static_cast<unsigned long>(ep->ExceptionRecord->ExceptionCode),
                ep->ExceptionRecord->ExceptionAddress,
                GetCurrentThreadId());
        fprintf(f, "All-thread stacks at crash point:\n");
        DumpAllThreadStacks(f);
        fclose(f);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ─── Hang watchdog ────────────────────────────────────────────────────────
// Spawned after successful INIT; stopped (via atomic) before SHUTDOWN join.
// Writes directly to crash.log via fprintf — NEVER via spdlog — so it works
// even when the spdlog async queue is itself deadlocked/frozen.
//
// Progress-gated: dumps ONLY while the engine reports Running AND the
// present counter hasn't advanced (a real stall). Idle/paused/stopped
// engines and healthy playback never dump — previously it dumped
// unconditionally every 5s forever (multi-MB crash.log of false positives
// plus a periodic all-thread stack-walk hitch during normal playback).
// At most kMaxStallDumps dumps per stall episode; crash.log is truncated
// past kMaxLogBytes instead of growing without bound.
static void DumpWatchdogStacks(int dumpCount) {
    constexpr long long kMaxLogBytes = 8LL * 1024 * 1024;
    const std::string path = BuildLogPath(L"crash.log");
    bool truncate = false;
    {
        FILE* probe = nullptr;
        if (fopen_s(&probe, path.c_str(), "ab") == 0 && probe) {
            fseek(probe, 0, SEEK_END);
            truncate = ftell(probe) > kMaxLogBytes;
            fclose(probe);
        }
    }
    FILE* f = nullptr;
    fopen_s(&f, path.c_str(), truncate ? "w" : "a");
    if (!f) return;
    SYSTEMTIME st;  GetLocalTime(&st);
    fprintf(f,
        "\n=== REX HANG WATCHDOG dump #%d  "
        "%04d-%02d-%02d %02d:%02d:%02d ===\n",
        dumpCount,
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    fprintf(f, "All thread stacks:\n");
    DumpAllThreadStacks(f);
    fprintf(f, "\n--- end of watchdog dump #%d ---\n", dumpCount);
    fclose(f);
}

// progress() returns {watched, presentCount}: watched=false when there is
// nothing worth watching (no engine, not Running), in which case no dump
// ever fires and stall state resets.
static void HangWatchdog(std::atomic<bool>& stop,
                         std::function<std::pair<bool, uint64_t>()> progress) {
    using namespace std::chrono;
    constexpr int kGraceSecs    = 10;
    constexpr int kIntervalSecs =  5;
    constexpr int kMaxStallDumps = 5;

    // Wait through grace period in 250ms slices for fast shutdown response.
    for (int i = 0; i < kGraceSecs * 4 && !stop.load(std::memory_order_acquire); ++i)
        std::this_thread::sleep_for(milliseconds(250));

    uint64_t lastCount = 0;
    bool haveLast = false;
    int stallDumps = 0;
    while (!stop.load(std::memory_order_acquire)) {
        auto [watched, count] = progress();
        if (!watched) {
            haveLast = false;
            stallDumps = 0;
        } else if (haveLast && count == lastCount) {
            if (stallDumps < kMaxStallDumps) {
                DumpWatchdogStacks(++stallDumps);
            }
            // else: already reported this episode; stay silent until
            // progress resumes (prevents log floods during long stalls).
        } else {
            haveLast = true;
            stallDumps = 0;
        }
        lastCount = count;

        // Sleep in slices for fast exit on shutdown.
        for (int i = 0; i < kIntervalSecs * 4 && !stop.load(std::memory_order_acquire); ++i)
            std::this_thread::sleep_for(milliseconds(250));
    }
}

static int SetImageWallpaper(const std::wstring& file, FillMode fillMode) {
    const wchar_t* subKey = L"Control Panel\\Desktop";

    int style = 10;
    bool tile = false;
    switch (fillMode) {
        case FillMode::Center: style = 0; break;
        case FillMode::Tile: style = 1; tile = true; break;
        case FillMode::Stretch: style = 2; break;
        case FillMode::Fit: style = 6; break;
        case FillMode::Fill:
        default: style = 10; break;
    }

    RegSetKeyValueW(HKEY_CURRENT_USER, subKey, L"Wallpaper", REG_SZ,
                    file.c_str(), (DWORD)((file.size() + 1) * sizeof(wchar_t)));
    const std::wstring styleStr = std::to_wstring(style);
    RegSetKeyValueW(HKEY_CURRENT_USER, subKey, L"WallpaperStyle", REG_SZ,
                    styleStr.c_str(), (DWORD)((styleStr.size() + 1) * sizeof(wchar_t)));
    const std::wstring tileStr = tile ? L"1" : L"0";
    RegSetKeyValueW(HKEY_CURRENT_USER, subKey, L"TileWallpaper", REG_SZ,
                    tileStr.c_str(), (DWORD)((tileStr.size() + 1) * sizeof(wchar_t)));

    if (!SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, (LPVOID)file.c_str(),
                               SPIF_UPDATEINIFILE | SPIF_SENDCHANGE)) {
        std::cerr << "REX: SystemParametersInfoW failed: " << GetLastError() << "\n";
        return 1;
    }
    return 0;
}

// â”€â”€â”€ Runtime frame protocol â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
//
// Persistent mode (`--runtime`): the process stays alive, reads length-
// prefixed JSON frames from stdin, and writes responses/events to stdout.
//
//   [uint32 LE length][UTF-8 JSON payload]
//
// Commands:  INIT, LOAD, PLAY, PAUSE, STOP, UNLOAD, SET_FILL_MODE,
//            SET_VOLUME, SET_SPEED, SET_LOOP, SET_MUTE, SET_MONITOR, GET_STATUS,
//            GET_CAPABILITIES, SHUTDOWN
// Responses: {"type":"response","id":N,"ok":true,"data":{...}}
// Events:    {"type":"event","event":"READY|LOADED|PLAYING|PAUSED|
//             STOPPED|UNLOADED|ERROR","data":{...}}

static bool ReadExact(HANDLE h, void* buffer, uint32_t size) {
    auto* dst = static_cast<BYTE*>(buffer);
    uint32_t got = 0;
    while (got < size) {
        DWORD read = 0;
        if (!ReadFile(h, dst + got, size - got, &read, nullptr)) return false;
        if (read == 0) return false;
        got += read;
    }
    return true;
}

static bool WriteExact(HANDLE h, const void* buffer, uint32_t size) {
    const auto* src = static_cast<const BYTE*>(buffer);
    uint32_t sent = 0;
    while (sent < size) {
        DWORD written = 0;
        if (!WriteFile(h, src + sent, size - sent, &written, nullptr)) return false;
        if (written == 0) return false;
        sent += written;
    }
    return true;
}

static std::mutex g_writeMutex;  // serializes frames from main + poll threads

static void WriteFrame(const std::string& frame) {
    std::lock_guard lock(g_writeMutex);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out == INVALID_HANDLE_VALUE || out == nullptr) return;
    uint32_t len = static_cast<uint32_t>(frame.size());
    WriteExact(out, &len, 4);
    WriteExact(out, frame.data(), len);
}

// P2-17: tri-state read. Zero/oversized prefixes are malformed input, not
// EOF — the dispatcher answers `malformed frame` and continues instead of
// tearing the session down. Only a closed/truncated stream ends the loop.
enum class ReadFrameResult { Ok, Eof, Malformed };

static ReadFrameResult ReadFrame(std::string& out) {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    if (in == INVALID_HANDLE_VALUE || in == nullptr) return ReadFrameResult::Eof;
    uint32_t len = 0;
    if (!ReadExact(in, &len, 4)) return ReadFrameResult::Eof;
    if (rex::wallpaper::ClassifyFrameLength(len) !=
        rex::wallpaper::FrameLengthVerdict::Ok) {
        return ReadFrameResult::Malformed;
    }
    out.assign(static_cast<size_t>(len), '\0');
    if (!ReadExact(in, out.data(), len)) return ReadFrameResult::Eof;
    return ReadFrameResult::Ok;
}

static json::Json Obj(std::initializer_list<std::pair<const char*, json::Json>> items) {
    json::Json::Object o;
    for (auto& [k, v] : items) {
        o.emplace(k, std::move(v));
    }
    return json::Json(std::move(o));
}

static void WriteResponse(int id, bool ok, const std::string& error = "",
                          const json::Json& data = json::Json(nullptr)) {
    json::Json::Object o;
    o.emplace("type", json::Json("response"));
    o.emplace("id", json::Json(id));
    o.emplace("ok", json::Json(ok));
    if (!error.empty()) o.emplace("error", json::Json(error));
    if (!data.IsNull()) o.emplace("data", data);
    WriteFrame(json::Json(std::move(o)).Dump());
}

static void WriteEvent(const std::string& event, const json::Json& data = json::Json(nullptr)) {
    json::Json::Object o;
    o.emplace("type", json::Json("event"));
    o.emplace("event", json::Json(event));
    if (!data.IsNull()) o.emplace("data", data);
    WriteFrame(json::Json(std::move(o)).Dump());
}

// ─── Client frame surface (cross-process preview) ─────────────────────────
//
// The host process sends its PID via SET_CLIENT_PID. The runtime then
// duplicates the shared fence + pool-slot NT handles into the host process
// (DuplicateHandle with DUPLICATE_SAME_ACCESS), so the host can call
// ID3D12Device::OpenSharedHandle on its own device. Handles are re-
// duplicated whenever the video surface generation changes (video re-open).
//
// 64-bit handle/fence values are transmitted as 0x-prefixed hex strings:
// the protocol's JSON numbers are doubles and would lose precision above 2^53.

// Client frame-surface state (R13): the lifecycle helpers live in
// wallpaper/runtime/ClientSurface.hpp so they are unit-testable; the
// command handlers below only wire them to the protocol.
static constexpr UINT kMaxPreviewSlots =
    rex::wallpaper::kMaxClientPreviewSlots;

static rex::wallpaper::ClientSurfaceState g_clientSurface;

static void CloseClientSurface() {
    rex::wallpaper::CloseSurfaceHandles(g_clientSurface);
}

static void DetachClientSurface() {
    rex::wallpaper::DetachClient(g_clientSurface);
}

static json::Json HexU64(uint64_t value) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%016llx",
                  static_cast<unsigned long long>(value));
    return json::Json(std::string(buf));
}

// ─── FRAME_READY poller ───────────────────────────────────────────────────
//
// Emits {"type":"event","event":"FRAME_READY",...} at up to ~30 Hz whenever
// a new frame is published. Frames carry the pool slot + fence value only;
// the host maps slot -> duplicated handle from the surface table received
// via GET_FRAME_SURFACE (handles are duplicated once per generation).

static void FramePollThread(std::mutex& engineGuard,
                            std::shared_ptr<IWallpaperEngine>& engine,
                            std::atomic<bool>& stop) {
    uint64_t lastIndex = 0;
    while (!stop.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
        IWallpaperEngine::FrameSurface s;
        bool shouldEmit = false;
        {
            std::lock_guard lock(engineGuard);
            if (engine) {
                s = engine->GetFrameSurface();
                if (s.available && s.frameIndex != lastIndex) {
                    lastIndex = s.frameIndex;
                    shouldEmit = true;
                }
            }
        }
        if (shouldEmit) {
            WriteEvent("FRAME_READY", Obj({
                { "slot",       json::Json(static_cast<double>(s.currentSlot)) },
                { "fenceValue", HexU64(s.currentFenceValue) },
                { "frameIndex", HexU64(s.frameIndex) },
                { "width",      json::Json(static_cast<double>(s.width)) },
                { "height",     json::Json(static_cast<double>(s.height)) },
            }));
        }
    }
}

// Telemetry sampler: appends a one-line snapshot to runtime-telemetry.log
// every 5s while an engine is alive. Cross-hardware baseline evidence in a
// single greppable line: process memory, adapter topology (decoder/renderer/
// display LUIDs), GPU segment usage and pipeline FPS.
static void TelemetryThread(std::mutex& engineGuard,
                            std::shared_ptr<IWallpaperEngine>& engine,
                            std::atomic<bool>& stop) {
    while (!stop.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        EngineTelemetry t;
        {
            std::lock_guard lock(engineGuard);
            if (!engine) continue;
            engine->GetTelemetry(t);
        }
        FILE* f = nullptr;
        if (fopen_s(&f, BuildLogPath(L"runtime-telemetry.log").c_str(), "a") != 0 || !f) {
            continue;
        }
        SYSTEMTIME st;
        GetLocalTime(&st);
        std::fprintf(f,
                     "%04u-%02u-%02u %02u:%02u:%02u | private=%.1fMB | "
                     "decRendSame=%d rendDispSame=%d | decLUID=%016llx rendLUID=%016llx | "
                     "gpuDed=%llu gpuShared=%llu gpuBudget=%llu | "
                      "decFps=%.1f rendFps=%.1f | slots=%llu ringBytes=%llu | presents=%llu | "
                      "srcFps=%.1f convFps=%.1f schedDrops=%llu repeat=%llu queue=%llu effFps=%.1f dispHz=%d\n",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                     t.privateBytesMB,
                     t.decoderRendererSameAdapter ? 1 : 0,
                     t.rendererDisplaySameAdapter ? 1 : 0,
                     static_cast<unsigned long long>(t.decoderAdapterLuid),
                     static_cast<unsigned long long>(t.rendererAdapterLuid),
                     static_cast<unsigned long long>(t.gpuDedicatedBytes),
                     static_cast<unsigned long long>(t.gpuSharedBytes),
                     static_cast<unsigned long long>(t.gpuBudgetBytes),
                      t.decoderFps, t.renderFps,
                      static_cast<unsigned long long>(t.ringSlotCount),
                      static_cast<unsigned long long>(t.ringBytes),
                      static_cast<unsigned long long>(t.presentCount),
                      t.sourceFps, t.convertFps,
                      static_cast<unsigned long long>(t.schedulerDrops),
                      static_cast<unsigned long long>(t.repeatedFrames),
                      static_cast<unsigned long long>(t.queueDepth),
                      t.effectiveVideoFps, t.displayHz);
        std::fclose(f);
    }
}


static int RunRuntime(int maxFps, bool hardwareAcceleration, bool forceSdr) {
    // stdout is owned by the frame protocol â€” log to file only.
    rex::shared::Logger::Instance().SetConsoleOutput(false);
    rex::shared::Logger::Instance().Initialize(BuildLogPath(L"runtime.log"), rex::shared::LogLevel::Trace);

    REX_LOG_INFO("Runtime", "Wallpaper runtime started (maxFps={}, hwAccel={})",
                 maxFps, hardwareAcceleration);

    // P3-G5: refuse a second concurrent runtime — two processes fight over
    // the WorkerW layer (verified: startup/shutdown of one silently kills
    // the other). Fail fast and loudly instead.
    const rex::wallpaper::RuntimeInstanceGuard instanceGuard;
    if (!instanceGuard.IsOwner()) {
        REX_LOG_ERROR("Runtime", "another wallpaper runtime already owns the "
                                 "desktop layer; refusing to start");
        std::fprintf(stderr, "REX: another wallpaper runtime is already running\n");
        return 3;
    }

    std::shared_ptr<IWallpaperEngine> engine;
    std::mutex engineGuard;   // guards engine assignment vs the FRAME_READY poller
    WallpaperConfig config;
    config.enableHardwareAcceleration = hardwareAcceleration;
    config.maxFps = maxFps;
    config.forceSdr = forceSdr;

    std::atomic<bool> framePollStop{false};
    std::thread framePollThread(FramePollThread, std::ref(engineGuard),
                                std::ref(engine), std::ref(framePollStop));

    // Hang watchdog: started on first INIT, stopped on SHUTDOWN.
    // Separate stop flag so the watchdog can be activated independently
    // of the frame poller (which runs from startup).
    std::atomic<bool> watchdogStop{true};  // starts stopped; set false on INIT
    std::thread watchdogThread;

    // Telemetry sampler: started on INIT success (engine alive), stopped
    // on SHUTDOWN / stdin EOF. Same lifecycle pattern as the watchdog.
    std::atomic<bool> telemetryStop{true};
    std::thread telemetryThread;

    std::string frame;
    while (true) {
        const ReadFrameResult fr = ReadFrame(frame);
        if (fr == ReadFrameResult::Eof) break;
        if (fr == ReadFrameResult::Malformed) {
            // P2-17: protocol violation, not death — answer and continue.
            REX_LOG_WARN("Runtime", "Malformed length prefix; continuing");
            WriteResponse(0, false, "malformed frame");
            continue;
        }
        auto parsed = json::Json::Parse(frame);
        if (!parsed) {
            REX_LOG_WARN("Runtime", "Malformed frame: {}", frame.c_str());
            WriteResponse(0, false, "malformed frame");
            continue;
        }

        const json::Json& req = *parsed;
        int id = req["id"].AsInt(0);
        std::string cmd = req["cmd"].AsString();
        const json::Json& params = req["params"];

        REX_LOG_DEBUG("Runtime", "Command: {} (id={})", cmd.c_str(), id);

        if (cmd == "SHUTDOWN") {
            // Stop the watchdog first (it holds no engine references, just
            // suspends threads and writes to crash.log).
            watchdogStop.store(true, std::memory_order_release);
            if (watchdogThread.joinable()) watchdogThread.join();

            // Telemetry reads engine state — must be gone before Shutdown.
            telemetryStop.store(true, std::memory_order_release);
            if (telemetryThread.joinable()) telemetryThread.join();

            framePollStop.store(true, std::memory_order_release);
            if (framePollThread.joinable()) {
                framePollThread.join();
            }
            DetachClientSurface();
            if (engine) engine->Shutdown();
            WriteResponse(id, true);
            REX_LOG_INFO("Runtime", "Wallpaper runtime shutdown complete");
            return 0;
        }

        if (cmd == "INIT") {
            if (engine) {
                WriteResponse(id, false, "already initialized");
                continue;
            }
            // P3-G5: protocol handshake. Absent version = pre-handshake
            // host, accepted; a mismatch fails loudly instead of running
            // on silent defaults.
            if (params.Has("protocolVersion") &&
                !rex::wallpaper::IsSupportedProtocolVersion(
                    params["protocolVersion"].AsInt(0))) {
                WriteResponse(id, false, "protocol version mismatch");
                continue;
            }
            config.maxFps = std::clamp(params["maxFps"].AsInt(config.maxFps), 0, 240);
            config.enableHardwareAcceleration =
                params["hardwareAcceleration"].AsBool(config.enableHardwareAcceleration);
            // Fullscreen auto-pause knob (default on). Exposed so tests and
            // a future Settings toggle can disable it; Electron sends it
            // once Settings wires "pause on fullscreen app".
            config.pauseOnFullscreen =
                params["pauseOnFullscreen"].AsBool(config.pauseOnFullscreen);
            {
                std::lock_guard lock(engineGuard);
                engine = CreateWallpaperEngine();
            }
            auto result = engine->Initialize(config);
            if (result.IsError()) {
                std::lock_guard lock(engineGuard);
                engine.reset();
                REX_LOG_ERROR("Runtime", "Engine init failed: {}", result.Error().message.c_str());
                WriteResponse(id, false, result.Error().message);
                continue;
            }
            // Persisted per-monitor assignments (EDID-keyed) are restored
            // on INIT, so a reconnected runtime resumes its wallpapers.
            if (auto settings = CreateSettingsService()) {
                engine->SetSettingsService(settings);
            }
            // Start the hang watchdog now that the engine is alive.
            // It will dump all thread stacks to crash.log at T+10s and
            // every 5s after — catching any freeze in the post-INIT window.
            if (watchdogThread.joinable()) {
                watchdogStop.store(true, std::memory_order_release);
                watchdogThread.join();
            }
            watchdogStop.store(false, std::memory_order_release);
            watchdogThread = std::thread(
                HangWatchdog, std::ref(watchdogStop),
                [&engineGuard, &engine]() -> std::pair<bool, uint64_t> {
                    // try_to_lock: never block the watchdog on the engine
                    // mutex (a deadlocked engine must not hang the watcher).
                    std::unique_lock<std::mutex> lock(engineGuard, std::try_to_lock);
                    if (!lock.owns_lock() || !engine) return {false, 0};
                    if (engine->GetState() != EngineState::Running) return {false, 0};
                    EngineTelemetry tel{};
                    engine->GetTelemetry(tel);
                    return {true, tel.presentCount};
                });

            // Start the 5s telemetry sampler now that the engine is alive.
            if (telemetryThread.joinable()) {
                telemetryStop.store(true, std::memory_order_release);
                telemetryThread.join();
            }
            telemetryStop.store(false, std::memory_order_release);
            telemetryThread = std::thread(TelemetryThread, std::ref(engineGuard),
                                          std::ref(engine), std::ref(telemetryStop));

            WriteResponse(id, true);
            WriteEvent("READY", Obj({ { "session", json::Json(engine->GetSessionId().ToString()) } }));
            continue;
        }

        if (cmd == "LOAD") {
            std::string path = params["path"].AsString();
            std::string type = params["type"].AsString("video");
            if (path.empty()) {
                WriteResponse(id, false, "LOAD requires a 'path'");
                continue;
            }
            if (!rex::wallpaper::SafeExists(Utf8ToWiden(path))) {
                WriteResponse(id, false, "file not found: " + path);
                continue;
            }

            FillMode mode = FillMode::Fill;
            if (auto m = ParseFillMode(params["mode"].AsString("fill"))) {
                mode = *m;
            }

            // Images go through the OS wallpaper API â€” no engine needed.
            if (type == "image" || type == "static") {
                // ALWAYS unload any engine scene first, regardless of
                // engine state. UnloadScene is idempotent, but skipping
                // it unless Running/Paused left the video backend alive
                // after a stall (Error) or STOP (Stopped) — and the next
                // recovery cycle reloaded the saved video scene right
                // over the new image (proven live 2026-10-09: image shows
                // ~1s, then the last video covers it).
                if (engine) {
                    auto unloadResult = engine->UnloadScene();
                    if (unloadResult.IsError()) {
                        REX_LOG_WARN("Runtime", "Unload before image load failed: {}",
                                     unloadResult.Error().message.c_str());
                    }
                }
                if (SetImageWallpaper(Utf8ToWiden(path), mode) != 0) {
                    WriteResponse(id, false, "SetImageWallpaper failed");
                    continue;
                }
                WriteResponse(id, true, "", Obj({ { "path", json::Json(path) },
                                                  { "type", json::Json("image") } }));
                WriteEvent("LOADED", Obj({ { "path", json::Json(path) },
                                           { "type", json::Json("image") } }));
                continue;
            }

            // Reject unknown types BEFORE unloading the running scene: a
            // doomed LOAD must never disturb the current wallpaper. The
            // engine re-validates (defense in depth); this keeps the
            // protocol response early and the desktop untouched.
            if (type != "video" && type != "audio") {
                WriteResponse(id, false,
                    "unsupported scene type '" + type + "' (supported: static, video, audio)");
                continue;
            }

            if (!engine) {
                WriteResponse(id, false, "runtime not initialized â€” send INIT first");
                continue;
            }

            // Scene swap: if a previous scene is still active, unload it
            // first so LoadScene's state guard (Initialized|Stopped) is
            // satisfied. Stop() alone would leave playback state behind;
            // UnloadScene resets the engine to Initialized for a clean load.
            EngineState currentState = engine->GetState();
            if (currentState == EngineState::Running || currentState == EngineState::Paused) {
                REX_LOG_INFO("Runtime", "Unloading active scene before load");
                auto unloadResult = engine->UnloadScene();
                if (unloadResult.IsError()) {
                    REX_LOG_WARN("Runtime", "Unload before load failed: {}",
                                 unloadResult.Error().message.c_str());
                }
            }

            SceneInfo scene;
            scene.id = "runtime";
            scene.name = "REX Wallpaper";
            scene.sourcePath = path;
            scene.type = type;
            scene.loop = params["loop"].AsBool(true);
            scene.muted = params["muted"].AsBool(true);
            scene.volume = params["volume"].AsFloat(1.0f);
            scene.speed = params["speed"].AsFloat(1.0f);
            scene.fillMode = mode;

            auto result = engine->LoadScene(scene);
            if (result.IsError()) {
                REX_LOG_ERROR("Runtime", "Scene load failed: {}", result.Error().message.c_str());
                WriteResponse(id, false, result.Error().message);
                continue;
            }

            // Assign the primary monitor by default.
            auto monitors = engine->EnumerateMonitors();
            for (const auto& monitor : monitors) {
                if (monitor.isPrimary) {
                    engine->AssignMonitor(monitor.id);
                    break;
                }
            }

            // Optional per-monitor wallpaper assignment: LOAD with a
            // "monitor" param pins this scene to that monitor (persisted
            // EDID-keyed; restored on the next INIT). Requires the
            // MultiMonitor desktop host to be active.
            std::string monitorId = params["monitor"].AsString();
            if (!monitorId.empty()) {
                auto assignResult = engine->SetPerMonitorWallpaper(monitorId, path);
                if (assignResult.IsError()) {
                    REX_LOG_WARN("Runtime",
                        "Per-monitor assignment for '{}' failed: {}",
                        monitorId.c_str(), assignResult.Error().message.c_str());
                }
            }

            WriteResponse(id, true, "", Obj({ { "path", json::Json(path) },
                                              { "type", json::Json(type) } }));
            WriteEvent("LOADED", Obj({ { "path", json::Json(path) },
                                       { "type", json::Json(type) } }));
            continue;
        }

        if (cmd == "PLAY" || cmd == "PAUSE" || cmd == "STOP" || cmd == "UNLOAD") {
            if (!engine) {
                WriteResponse(id, false, "runtime not initialized");
                continue;
            }
            std::string eventName;
            // Only PLAY may call Play(). It used to run unconditionally before
            // the branch below, so PAUSE/STOP/UNLOAD briefly resumed decode and
            // audio and re-stamped the stopwatch — playback position then
            // reported as ~0 instead of where the user left it.
            rex::shared::Result<void> result;
            if (cmd == "PLAY") {
                result = engine->Play();
                eventName = "PLAYING";
            } else if (cmd == "PAUSE") {
                result = engine->Pause();
                eventName = "PAUSED";
            } else if (cmd == "STOP") {
                result = engine->Stop();
                eventName = "STOPPED";
            } else {
                result = engine->UnloadScene();
                eventName = "UNLOADED";
            }
            if (result.IsError()) {
                WriteResponse(id, false, result.Error().message);
                continue;
            }
            WriteResponse(id, true);
            WriteEvent(eventName);
            continue;
        }

        if (cmd == "SET_FILL_MODE" || cmd == "SET_VOLUME" || cmd == "SET_SPEED" ||
            cmd == "SET_LOOP" || cmd == "SET_MUTE" || cmd == "SET_BRIGHTNESS" || cmd == "SET_CONTRAST" ||
            cmd == "SET_SATURATION") {
            if (!engine) {
                WriteResponse(id, false, "runtime not initialized");
                continue;
            }
            std::string property;
            std::string value;
            if (cmd == "SET_FILL_MODE") {
                property = "fillMode";
                value = params["value"].AsString();
            } else if (cmd == "SET_VOLUME") {
                property = "volume";
                value = std::to_string(params["value"].AsFloat(0.0f));
            } else if (cmd == "SET_SPEED") {
                property = "speed";
                value = std::to_string(params["value"].AsFloat(0.0f));
            } else if (cmd == "SET_LOOP") {
                property = "loop";
                value = params["value"].AsBool(false) ? "true" : "false";
            } else if (cmd == "SET_MUTE") {
                property = "mute";
                value = params["value"].AsBool(false) ? "true" : "false";
            } else if (cmd == "SET_BRIGHTNESS") {
                property = "brightness";
                value = std::to_string(params["value"].AsFloat(0.0f));
            } else if (cmd == "SET_CONTRAST") {
                property = "contrast";
                value = std::to_string(params["value"].AsFloat(0.0f));
            } else {
                property = "saturation";
                value = std::to_string(params["value"].AsFloat(0.0f));
            }

            auto result = engine->SetProperty(property, value);
            if (result.IsError()) {
                REX_LOG_WARN("Runtime", "{} failed: {}", cmd.c_str(), result.Error().message.c_str());
                WriteResponse(id, false, result.Error().message);
                WriteEvent("ERROR", Obj({ { "command", json::Json(cmd) },
                                          { "error", json::Json(result.Error().message) } }));
                continue;
            }
            WriteResponse(id, true);
            continue;
        }

        if (cmd == "SET_MONITOR") {
            if (!engine) {
                WriteResponse(id, false, "runtime not initialized");
                continue;
            }
            std::string monitorId = params["id"].AsString();
            auto result = engine->AssignMonitor(monitorId);
            if (result.IsError()) {
                WriteResponse(id, false, result.Error().message);
                continue;
            }
            WriteResponse(id, true);
            continue;
        }

        if (cmd == "SET_CLIENT_PID") {
            const DWORD pid = static_cast<DWORD>(params["pid"].AsInt(0));
            if (pid == 0) {
                WriteResponse(id, false, "SET_CLIENT_PID requires a 'pid'");
                continue;
            }
            DetachClientSurface();
            g_clientSurface.process = OpenProcess(
                PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!g_clientSurface.process) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "0x%08lx",
                              static_cast<unsigned long>(GetLastError()));
                WriteResponse(id, false,
                              std::string("cannot open client process (") + buf + ")");
                continue;
            }
            g_clientSurface.pid = pid;
            REX_LOG_INFO("Runtime", "client frame surface bound to pid {}", pid);
            WriteResponse(id, true);
            continue;
        }

        if (cmd == "DETACH_CLIENT") {
            // R13: explicit detach — drops the surface handles AND the
            // process binding (CloseClientSurface alone keeps pid/process).
            DetachClientSurface();
            REX_LOG_INFO("Runtime", "client frame surface detached");
            WriteResponse(id, true);
            continue;
        }

        if (cmd == "GET_FRAME_SURFACE") {
            if (!engine) {
                WriteResponse(id, false, "runtime not initialized — send INIT first");
                continue;
            }
            const IWallpaperEngine::FrameSurface s = engine->GetFrameSurface();
            if (!s.available) {
                WriteResponse(id, true, "", Obj({ { "available", json::Json(false) } }));
                continue;
            }

            // (Re-)duplicate the fence + slot handles when the video surface
            // changed since the last request. Without a client PID the
            // process-local values are returned (self-preview/debugging).
            if (s.generation != g_clientSurface.generation ||
                g_clientSurface.fence == nullptr) {
                CloseClientSurface();
                // R13: a bound client that exited leaves a signaled process
                // handle — duplicating into it yields stale handles, so
                // detach instead of serializing them as valid.
                if (g_clientSurface.process &&
                    !rex::wallpaper::IsProcessAlive(g_clientSurface.process)) {
                    REX_LOG_WARN("Runtime", "client pid {} exited; detaching frame surface",
                                 g_clientSurface.pid);
                    DetachClientSurface();
                    WriteResponse(id, false,
                                  "client process exited (surface detached)");
                    continue;
                }
                g_clientSurface.generation = s.generation;
                g_clientSurface.slotCount = std::min<UINT>(s.slotCount, kMaxPreviewSlots);
                if (g_clientSurface.process) {
                    const BOOL fenceOk = DuplicateHandle(GetCurrentProcess(),
                                    reinterpret_cast<HANDLE>(s.fenceHandle),
                                    g_clientSurface.process, &g_clientSurface.fence,
                                    0, FALSE, DUPLICATE_SAME_ACCESS);
                    BOOL slotsOk = TRUE;
                    for (UINT i = 0; i < g_clientSurface.slotCount; ++i) {
                        const HANDLE local = reinterpret_cast<HANDLE>(engine->GetFrameSlotHandle(i));
                        if (!local) continue;
                        slotsOk &= DuplicateHandle(GetCurrentProcess(), local,
                                        g_clientSurface.process, &g_clientSurface.slots[i],
                                        0, FALSE, DUPLICATE_SAME_ACCESS);
                    }
                    g_clientSurface.duplicated = true;
                    // R13: never serialize a NULL handle as valid — drop the
                    // surface and report failure instead.
                    if (!fenceOk || !slotsOk ||
                        !rex::wallpaper::ValidateDuplicatedSurface(
                            g_clientSurface.fence, g_clientSurface.slots,
                            g_clientSurface.slotCount)) {
                        REX_LOG_WARN("Runtime",
                                     "client handle duplication failed; dropping surface");
                        CloseClientSurface();
                        WriteResponse(id, false,
                                      "client handle duplication failed");
                        continue;
                    }
                } else {
                    g_clientSurface.fence = reinterpret_cast<HANDLE>(s.fenceHandle);
                    for (UINT i = 0; i < g_clientSurface.slotCount; ++i) {
                        g_clientSurface.slots[i] =
                            reinterpret_cast<HANDLE>(engine->GetFrameSlotHandle(i));
                    }
                    g_clientSurface.duplicated = false;
                }
            }

            json::Json::Object data;
            data.emplace("available", json::Json(true));
            data.emplace("fenceHandle", HexU64(reinterpret_cast<uint64_t>(g_clientSurface.fence)));
            data.emplace("adapterLuid", HexU64(s.adapterLuid));
            data.emplace("slotCount", json::Json(static_cast<double>(s.slotCount)));
            data.emplace("currentSlot", json::Json(static_cast<double>(s.currentSlot)));
            data.emplace("currentFenceValue", HexU64(s.currentFenceValue));
            data.emplace("width", json::Json(static_cast<double>(s.width)));
            data.emplace("height", json::Json(static_cast<double>(s.height)));
            data.emplace("frameIndex", HexU64(s.frameIndex));
            data.emplace("generation", HexU64(s.generation));
            json::Json::Array slots;
            for (UINT i = 0; i < g_clientSurface.slotCount; ++i) {
                json::Json::Object slot;
                slot.emplace("slot", json::Json(static_cast<double>(i)));
                slot.emplace("handle", HexU64(reinterpret_cast<uint64_t>(g_clientSurface.slots[i])));
                slots.emplace_back(json::Json(std::move(slot)));
            }
            data.emplace("slots", json::Json(std::move(slots)));
            WriteResponse(id, true, "", json::Json(std::move(data)));
            continue;
        }

        if (cmd == "GET_STATUS") {
            if (!engine) {
                WriteResponse(id, false, "runtime not initialized");
                continue;
            }
            json::Json::Object data;
            data.emplace("state", json::Json(static_cast<int>(engine->GetState())));
            data.emplace("playing", json::Json(engine->IsPlaying()));
            data.emplace("position", json::Json(engine->GetPosition()));
            data.emplace("duration", json::Json(engine->GetDuration()));
            data.emplace("renderer", json::Json(engine->GetRendererType()));
            data.emplace("privateBytes", json::Json(GetProcessPrivateMemoryMB()));

            // â”€â”€ Telemetry â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
            EngineTelemetry t;
            engine->GetTelemetry(t);
            json::Json::Object tel;
            tel.emplace("framesDecoded", json::Json(static_cast<double>(t.framesDecoded)));
            tel.emplace("framesConsumed", json::Json(static_cast<double>(t.framesConsumed)));
            tel.emplace("framesDropped", json::Json(static_cast<double>(t.framesDropped)));
            tel.emplace("decoderFps", json::Json(t.decoderFps));
            tel.emplace("renderFps", json::Json(t.renderFps));
            tel.emplace("videoWidth", json::Json(t.videoWidth));
            tel.emplace("videoHeight", json::Json(t.videoHeight));
            tel.emplace("ringSlotCount", json::Json(static_cast<double>(t.ringSlotCount)));
            tel.emplace("ringBytes", json::Json(static_cast<double>(t.ringBytes)));
            tel.emplace("textureBytes", json::Json(static_cast<double>(t.textureBytes)));
            tel.emplace("uploadBytes", json::Json(static_cast<double>(t.uploadBytes)));
            tel.emplace("textureRecreations", json::Json(static_cast<double>(t.textureRecreations)));
            tel.emplace("uploads", json::Json(static_cast<double>(t.uploads)));
            tel.emplace("allocatorsPerFrame", json::Json(static_cast<double>(t.allocatorsPerFrame)));
            tel.emplace("scenesLoaded", json::Json(static_cast<double>(t.scenesLoaded)));
            tel.emplace("scenesReleased", json::Json(static_cast<double>(t.scenesReleased)));
            tel.emplace("decodeWidth", json::Json(t.decodeWidth));
            tel.emplace("decodeHeight", json::Json(t.decodeHeight));
            tel.emplace("hdrActive", json::Json(t.hdrActive));
            tel.emplace("displayMaxNits", json::Json(t.displayMaxNits));
            tel.emplace("gpuDedicated", json::Json(static_cast<double>(t.gpuDedicatedBytes)));
            tel.emplace("gpuShared", json::Json(static_cast<double>(t.gpuSharedBytes)));
            tel.emplace("gpuBudget", json::Json(static_cast<double>(t.gpuBudgetBytes)));
            tel.emplace("decRendSameAdapter", json::Json(t.decoderRendererSameAdapter));
            tel.emplace("rendDispSameAdapter", json::Json(t.rendererDisplaySameAdapter));
            tel.emplace("decoderLuid", json::Json(HexU64(t.decoderAdapterLuid)));
            tel.emplace("rendererLuid", json::Json(HexU64(t.rendererAdapterLuid)));
            tel.emplace("presentCount", json::Json(static_cast<double>(t.presentCount)));
            tel.emplace("sourceFps", json::Json(t.sourceFps));
            tel.emplace("convertFps", json::Json(t.convertFps));
            tel.emplace("schedulerDrops", json::Json(static_cast<double>(t.schedulerDrops)));
            tel.emplace("presentFps", json::Json(t.presentFps));
            tel.emplace("repeatedFrames", json::Json(static_cast<double>(t.repeatedFrames)));
            tel.emplace("queueDepth", json::Json(static_cast<double>(t.queueDepth)));
            tel.emplace("pacingMs", json::Json(t.pacingMs));
            tel.emplace("effectiveVideoFps", json::Json(t.effectiveVideoFps));
            tel.emplace("playbackSpeed", json::Json(t.playbackSpeed));
            tel.emplace("displayHz", json::Json(t.displayHz));
            tel.emplace("decodeMsP50", json::Json(t.decodeMsP50));
            tel.emplace("decodeMsP95", json::Json(t.decodeMsP95));
            tel.emplace("decodeMsP99", json::Json(t.decodeMsP99));
            tel.emplace("vpMsP50", json::Json(t.vpMsP50));
            tel.emplace("vpMsP95", json::Json(t.vpMsP95));
            tel.emplace("vpMsP99", json::Json(t.vpMsP99));
            data.emplace("telemetry", json::Json(std::move(tel)));
            SceneInfo scene = engine->GetCurrentScene();
            json::Json::Object sceneObj;
            sceneObj.emplace("path", json::Json(scene.sourcePath));
            sceneObj.emplace("type", json::Json(scene.type));
            sceneObj.emplace("name", json::Json(scene.name));
            sceneObj.emplace("loop", json::Json(scene.loop));
            sceneObj.emplace("muted", json::Json(scene.muted));
            sceneObj.emplace("volume", json::Json(scene.volume));
            sceneObj.emplace("speed", json::Json(scene.speed));
            sceneObj.emplace("fillMode", json::Json(std::string(ToString(scene.fillMode))));
            data.emplace("scene", json::Json(std::move(sceneObj)));
            WriteResponse(id, true, "", json::Json(std::move(data)));
            continue;
        }

        if (cmd == "GET_CAPABILITIES") {
            json::Json::Object data;
            data.emplace("renderer", json::Json(engine ? engine->GetRendererType() : "none"));
            json::Json::Array props;
            if (engine) {
                for (const auto& p : engine->GetSupportedProperties()) {
                    props.emplace_back(json::Json(p));
                }
            }
            data.emplace("properties", json::Json(std::move(props)));
            WriteResponse(id, true, "", json::Json(std::move(data)));
            continue;
        }

        WriteResponse(id, false, "unknown command: " + cmd);
    }

    // stdin closed (host quit without SHUTDOWN) â€” clean up.
    // Mirror the SHUTDOWN path so both watchdog and frame-poll threads
    // are stopped/joined before any std::thread members are destroyed.
    watchdogStop.store(true, std::memory_order_release);
    if (watchdogThread.joinable()) {
        watchdogThread.join();
    }
    telemetryStop.store(true, std::memory_order_release);
    if (telemetryThread.joinable()) {
        telemetryThread.join();
    }
    framePollStop.store(true, std::memory_order_release);
    if (framePollThread.joinable()) {
        framePollThread.join();
    }
    DetachClientSurface();
    if (engine) engine->Shutdown();
    REX_LOG_INFO("Runtime", "Wallpaper runtime exiting (stdin closed)");
    return 0;
}

// ─── Benchmark mode: FFmpeg D3D11VA -> D3D12 zero-copy interop ──────────
//
// Decodes a video with FFmpeg's D3D11VA hwaccel and renders the decoded NV12
// frames through D3D12 without any CPU round-trip:
//
//   FFmpeg D3D11VA pool texture (NV12 array, SHARED|SHARED_NTHANDLE)
//     -> IDXGIResource1::CreateSharedHandle (NT handle)
//     -> ID3D12Device::OpenSharedHandle
//     -> R8 (Y) / R8G8 (UV) plane SRVs of the array slice
//     -> fullscreen triangle shader (BT.709 limited range) -> swap chain
//
// The decode textures are created by FFmpeg with the flags we set on
// AVD3D11VADeviceContext (BindFlags/MiscFlags), so the D3D12 device consumes
// the decoder's own pool surfaces directly.

namespace {

constexpr char kBenchmarkVs[] = R"(
struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};
VSOut main(uint id : SV_VertexID) {
    VSOut o;
    float2 pos[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };
    o.pos = float4(pos[id], 0.0, 1.0);
    o.uv  = o.pos.xy * 0.5 + 0.5;
    return o;
}
)";

constexpr char kBenchmarkPs[] = R"(
Texture2D   tex  : register(t0);
SamplerState sam  : register(s0);
struct PSIn {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};
float4 main(PSIn input) : SV_TARGET {
    return float4(tex.Sample(sam, input.uv).rgb, 1.0);
}
)";

// P13: explicit decode-path selection. Auto = try HW, fall back to SW.
// HwOnly aborts if the codec cannot be hardware-decoded; SwOnly never
// creates a D3D11VA context.
enum class HwMode { Auto, HwOnly, SwOnly };

struct BenchFlags {
    std::wstring file;
    double seekSec = 0.0;
    int maxFrames = 0;  // 0 = decode until EOF
    HwMode hwMode = HwMode::Auto;
};

struct BenchStats {
    int64_t frames = 0;
    int64_t decodeErrors = 0;
    int64_t shareErrors = 0;
    double startupMs = 0.0;
    double seekMs = 0.0;
    double decodeAvgMs = 0.0, decodeMaxMs = 0.0;
    double shareAvgMs = 0.0, shareMaxMs = 0.0;
    double renderAvgMs = 0.0, renderMaxMs = 0.0;
    double e2eAvgMs = 0.0, e2eMaxMs = 0.0;
    double totalMs = 0.0;
    double fps = 0.0;
    double gpuMemMB = 0.0, gpuBudgetMB = 0.0;
    double processMemMB = 0.0;
    std::string container;
    std::string codec;
    int width = 0, height = 0;
    int containerFps = 0;
};

bool CompileBenchmarkShader(const char* source, size_t size, const char* target,
                            Microsoft::WRL::ComPtr<ID3DBlob>& out) {
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    Microsoft::WRL::ComPtr<ID3DBlob> error;
    HRESULT hr = D3DCompile(source, size, nullptr, nullptr, nullptr, "main",
                            target, flags, 0, &out, &error);
    if (FAILED(hr)) {
        if (error) {
            fprintf(stderr, "REX: shader compile error:\n%.*s\n",
                    static_cast<int>(error->GetBufferSize()),
                    static_cast<const char*>(error->GetBufferPointer()));
        }
        return false;
    }
    return true;
}

}  // namespace

static int RunBenchmark(const BenchFlags& flags) {
    using Clock = std::chrono::steady_clock;
    using ms = std::chrono::duration<double, std::milli>;

    if (flags.file.empty()) {
        std::cerr << "REX: --benchmark requires --file <video>\n";
        return 2;
    }
    if (!rex::wallpaper::SafeExists(flags.file)) {
        std::cerr << "REX: benchmark file not found: "
                  << WidenToUtf8(flags.file) << "\n";
        return 2;
    }

    BenchStats stats;
    stats.processMemMB = GetProcessPrivateMemoryMB();
    const auto tStart = Clock::now();

    // Hidden test window drives the swap chain / present pacing.
    const wchar_t* kBenchWindowClass = L"REXBenchmarkWindow";
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kBenchWindowClass;
    RegisterClassExW(&wc);
    const int winW = 1280;
    const int winH = 720;
    HWND hwnd = CreateWindowExW(0, kBenchWindowClass, L"REX Benchmark",
                                WS_POPUP | WS_VISIBLE,
                                0, 0, winW, winH, nullptr, nullptr, wc.hInstance,
                                nullptr);
    if (!hwnd) {
        std::cerr << "REX: benchmark window creation failed\n";
        return 1;
    }

    // D3D12 device (rex::graphics wrapper).
    rex::graphics::Device d3d12;
    if (d3d12.Initialize().IsError()) {
        std::cerr << "REX: d3d12 device init failed\n";
        DestroyWindow(hwnd);
        return 1;
    }

    // D3D11 device on the SAME adapter: hardware video decode + shared textures.
    Microsoft::WRL::ComPtr<ID3D11Device> d3d11;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d11Ctx;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> vidDev;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> vidCtx;
    Microsoft::WRL::ComPtr<ID3D11VideoContext1> vidCtx1;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> vidEnum;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> vidProc;
    Microsoft::WRL::ComPtr<ID3D11Device5> d3d11d5;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> d3d11ctx4;
    Microsoft::WRL::ComPtr<ID3D11Fence> fence11;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence12;
    HANDLE fenceHandle = nullptr;
    UINT64 fenceValue = 0;
    UINT64 slotConsume[8] = {};   // D3D11 -> D3D12: blt into slot done (wait before draw)
    UINT64 d12SignalVal = 0;
    {
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        if (!d3d12.Adapter() || FAILED(d3d12.Adapter().As(&adapter))) {
            std::cerr << "REX: no adapter for d3d11 device\n";
            DestroyWindow(hwnd);
            return 1;
        }
        UINT createFlags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                       createFlags, &fl, 1, D3D11_SDK_VERSION,
                                       &d3d11, nullptr, &d3d11Ctx);
        if (FAILED(hr)) {
            std::cerr << "REX: d3d11 device creation failed (0x" << std::hex
                      << static_cast<unsigned>(hr) << std::dec << ")\n";
            DestroyWindow(hwnd);
            return 1;
        }
        if (FAILED(d3d11.As(&vidDev)) || FAILED(d3d11Ctx.As(&vidCtx)) ||
            FAILED(d3d11Ctx.As(&vidCtx1))) {
            std::cerr << "REX: d3d11 video device/context missing\n";
            DestroyWindow(hwnd);
            return 1;
        }

        // Shared fence: orders D3D11 (video engine blt) before D3D12 (3D
        // engine draw) on the same shared texture.
        HRESULT hrF = d3d12.Get()->CreateFence(0, D3D12_FENCE_FLAG_SHARED,
                                               IID_PPV_ARGS(&fence12));
        if (SUCCEEDED(hrF)) {
            hrF = d3d12.Get()->CreateSharedHandle(
                fence12.Get(), nullptr, GENERIC_ALL, nullptr, &fenceHandle);
        }
        if (SUCCEEDED(hrF)) {
            if (FAILED(d3d11.As(&d3d11d5)) ||
                FAILED(d3d11Ctx.As(&d3d11ctx4))) {
                hrF = E_FAIL;
            } else {
                hrF = d3d11d5->OpenSharedFence(fenceHandle, IID_PPV_ARGS(&fence11));
            }
        }
        if (FAILED(hrF)) {
            std::cerr << "REX: shared fence setup failed (0x" << std::hex
                      << static_cast<unsigned>(hrF) << std::dec << ")\n";
            DestroyWindow(hwnd);
            return 1;
        }
    }

    // ── FFmpeg: open input ──
    av_log_set_level(AV_LOG_ERROR);
    const std::string pathUtf8 = WidenToUtf8(flags.file);
    AVFormatContext* fmt = nullptr;
    int ret = avformat_open_input(&fmt, pathUtf8.c_str(), nullptr, nullptr);
    if (ret < 0) {
        char buf[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(ret, buf, sizeof(buf));
        std::cerr << "REX: cannot open input: " << buf << "\n";
        DestroyWindow(hwnd);
        return 1;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        std::cerr << "REX: cannot read stream info\n";
        avformat_close_input(&fmt);
        DestroyWindow(hwnd);
        return 1;
    }
    const int vIdx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vIdx < 0) {
        std::cerr << "REX: no video stream found\n";
        avformat_close_input(&fmt);
        DestroyWindow(hwnd);
        return 1;
    }
    AVStream* stream = fmt->streams[vIdx];
    stats.container = fmt->iformat->name;
    stats.width = stream->codecpar->width;
    stats.height = stream->codecpar->height;
    if (stream->r_frame_rate.den > 0) {
        stats.containerFps = static_cast<int>(0.5 +
            static_cast<double>(stream->r_frame_rate.num) / stream->r_frame_rate.den);
    }

    // ── Decoder with D3D11VA on our D3D11 device ──
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        std::cerr << "REX: no decoder for codec "
                  << avcodec_get_name(stream->codecpar->codec_id) << "\n";
        avformat_close_input(&fmt);
        DestroyWindow(hwnd);
        return 1;
    }
    stats.codec = codec->name;
    AVCodecContext* dec = avcodec_alloc_context3(codec);
    if (!dec) {
        std::cerr << "REX: out of memory (codec ctx)\n";
        avformat_close_input(&fmt);
        DestroyWindow(hwnd);
        return 1;
    }
    if (avcodec_parameters_to_context(dec, stream->codecpar) < 0) {
        std::cerr << "REX: cannot copy codec parameters\n";
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        DestroyWindow(hwnd);
        return 1;
    }
    dec->thread_count = 1;  // deterministic sequential decode (wallpaper-style)
    dec->pkt_timebase = stream->time_base;

    // D3D11VA device context wrapping our D3D11 device. Textures are created
    // with SHARED | SHARED_NTHANDLE so the decode pool is openable in D3D12.
    // --sw skips this entirely (no D3D11VA context is ever created).
    AVBufferRef* hwDevRef = nullptr;
    if (flags.hwMode != HwMode::SwOnly) {
        hwDevRef = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (!hwDevRef) {
            std::cerr << "REX: out of memory (hw device ctx)\n";
            avcodec_free_context(&dec);
            avformat_close_input(&fmt);
            DestroyWindow(hwnd);
            return 1;
        }
        {
            AVHWDeviceContext* hwDev = reinterpret_cast<AVHWDeviceContext*>(hwDevRef->data);
            AVD3D11VADeviceContext* hwctx =
                reinterpret_cast<AVD3D11VADeviceContext*>(hwDev->hwctx);
            hwctx->device = d3d11.Get();
            hwctx->BindFlags = D3D11_BIND_SHADER_RESOURCE;  // FFmpeg adds BIND_DECODER
            hwctx->MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        }
        if (av_hwdevice_ctx_init(hwDevRef) < 0) {
            std::cerr << "REX: d3d11va device init failed\n";
            av_buffer_unref(&hwDevRef);
            avcodec_free_context(&dec);
            avformat_close_input(&fmt);
            DestroyWindow(hwnd);
            return 1;
        }
        dec->hw_device_ctx = av_buffer_ref(hwDevRef);
    }
    if (avcodec_open2(dec, codec, nullptr) < 0) {
        std::cerr << "REX: decoder open failed (codec/hwaccel unsupported?)\n";
        av_buffer_unref(&hwDevRef);
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        DestroyWindow(hwnd);
        return 1;
    }
    if (flags.hwMode == HwMode::HwOnly && !dec->hw_device_ctx) {
        std::cerr << "REX: --hw specified but hardware decode not active\n";
        av_buffer_unref(&hwDevRef);
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        DestroyWindow(hwnd);
        return 1;
    }

    AVFrame* frame = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    if (!frame || !pkt) {
        av_frame_free(&frame);
        av_packet_free(&pkt);
        av_buffer_unref(&hwDevRef);
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        DestroyWindow(hwnd);
        return 1;
    }

    // ── D3D12 queue, swap chain, pipeline ──
    rex::graphics::CommandQueue queue;
    if (queue.Initialize(d3d12.Get()).IsError()) {
        std::cerr << "REX: command queue init failed\n";
        av_frame_free(&frame);
        av_packet_free(&pkt);
        av_buffer_unref(&hwDevRef);
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        DestroyWindow(hwnd);
        return 1;
    }
    rex::graphics::SwapChain swap;
    if (swap.Initialize(queue.Get(), d3d12.Get(), hwnd, winW, winH).IsError()) {
        std::cerr << "REX: swap chain init failed\n";
        queue.Shutdown();
        av_frame_free(&frame);
        av_packet_free(&pkt);
        av_buffer_unref(&hwDevRef);
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        DestroyWindow(hwnd);
        return 1;
    }

    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, psBlob;
    if (!CompileBenchmarkShader(kBenchmarkVs, sizeof(kBenchmarkVs) - 1, "vs_5_0", vsBlob) ||
        !CompileBenchmarkShader(kBenchmarkPs, sizeof(kBenchmarkPs) - 1, "ps_5_0", psBlob)) {
        std::cerr << "REX: shader compilation failed\n";
        swap.Shutdown();
        queue.Shutdown();
        av_frame_free(&frame);
        av_packet_free(&pkt);
        av_buffer_unref(&hwDevRef);
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        DestroyWindow(hwnd);
        return 1;
    }

    // Root signature: t0 SRV table (pixel) + linear clamp sampler.
    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    range.RegisterSpace = 0;
    range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER rootParam = {};
    rootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParam.DescriptorTable.NumDescriptorRanges = 1;
    rootParam.DescriptorTable.pDescriptorRanges = &range;
    rootParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = 1;
    rsDesc.pParameters = &rootParam;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers = &sampler;
    Microsoft::WRL::ComPtr<ID3DBlob> rsBlob, rsError;
    if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                           &rsBlob, &rsError))) {
        std::cerr << "REX: root signature serialization failed\n";
        swap.Shutdown();
        queue.Shutdown();
        av_frame_free(&frame);
        av_packet_free(&pkt);
        av_buffer_unref(&hwDevRef);
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        DestroyWindow(hwnd);
        return 1;
    }
    auto rootSig = d3d12.CreateRootSignature(rsBlob->GetBufferPointer(), rsBlob->GetBufferSize());
    if (!rootSig) {
        std::cerr << "REX: root signature creation failed\n";
        swap.Shutdown();
        queue.Shutdown();
        av_frame_free(&frame);
        av_packet_free(&pkt);
        av_buffer_unref(&hwDevRef);
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        DestroyWindow(hwnd);
        return 1;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = rootSig.Get();
    psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
    psoDesc.RasterizerState = D3D12_RASTERIZER_DESC{};
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.DepthStencilState = D3D12_DEPTH_STENCIL_DESC{};
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    auto pso = d3d12.CreateGraphicsPipelineState(psoDesc);
    if (!pso) {
        std::cerr << "REX: pipeline state creation failed\n";
        swap.Shutdown();
        queue.Shutdown();
        av_frame_free(&frame);
        av_packet_free(&pkt);
        av_buffer_unref(&hwDevRef);
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        DestroyWindow(hwnd);
        return 1;
    }

    // Double-buffered SRV heap (Y + UV per frame) so CPU descriptor writes
    // never race the GPU sampling the previous frame.
    const UINT srvInc = d3d12.GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeaps[2];
    for (int i = 0; i < 2; ++i) {
        srvHeaps[i] = d3d12.CreateDescriptorHeap(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2,
            D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
        if (!srvHeaps[i]) {
            std::cerr << "REX: descriptor heap creation failed\n";
            swap.Shutdown();
            queue.Shutdown();
            av_frame_free(&frame);
            av_packet_free(&pkt);
            av_buffer_unref(&hwDevRef);
            avcodec_free_context(&dec);
            avformat_close_input(&fmt);
            DestroyWindow(hwnd);
            return 1;
        }
    }
    auto allocator = d3d12.CreateCommandAllocator();
    auto cmdList = d3d12.CreateCommandList(allocator.Get());
    if (!allocator || !cmdList) {
        std::cerr << "REX: command list creation failed\n";
        swap.Shutdown();
        queue.Shutdown();
        av_frame_free(&frame);
        av_packet_free(&pkt);
        av_buffer_unref(&hwDevRef);
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        DestroyWindow(hwnd);
        return 1;
    }

    // ── Seek (optional) ──
    if (flags.seekSec > 0.0) {
        const auto tSeek = Clock::now();
        const int64_t ts = av_rescale_q(
            static_cast<int64_t>(flags.seekSec * AV_TIME_BASE), AV_TIME_BASE_Q,
            stream->time_base);
        if (av_seek_frame(fmt, vIdx, ts, AVSEEK_FLAG_BACKWARD) >= 0) {
            avcodec_flush_buffers(dec);
        } else {
            std::cerr << "REX: seek to " << flags.seekSec << "s failed (continuing from start)\n";
        }
        stats.seekMs = ms(Clock::now() - tSeek).count();
    }

    // ── Decode + present loop ──
    double decodeSum = 0.0, decodeMax = 0.0;
    double shareSum = 0.0, shareMax = 0.0;
    double renderSum = 0.0, renderMax = 0.0;
    double e2eSum = 0.0, e2eMax = 0.0;
    bool sawFirst = false;
    bool fatal = false;

    // Shared pool for GPU-copied frames. FFmpeg's decode textures carry
    // D3D11_BIND_DECODER, which D3D11 refuses to share (CreateSharedHandle
    // -> DXGI_ERROR_INVALID_CALL even with SHARED|SHARED_NTHANDLE), so we
    // copy each decoded frame into our own shareable NV12 textures (GPU-only
    // CopySubresourceRegion; no CPU round-trip) and share those with D3D12.
    constexpr int kBenchPoolSize = 8;
    std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> poolTex;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> poolRes12;
    std::vector<HANDLE> poolHandles;
    int poolW = 0, poolH = 0;
    int poolNext = 0;

    auto handleDecodedFrame = [&](const Clock::time_point& tDec0) -> bool {
        const auto tDec1 = Clock::now();
        const double decodeMs = ms(tDec1 - tDec0).count();

        // --sw: decode-only accounting. Everything below (VP blt, shared
        // NT handles, D3D12 open/render) is inherently D3D11-texture
        // interop, so software frames go straight to the stats.
        if (frame->format != AV_PIX_FMT_D3D11 && flags.hwMode == HwMode::SwOnly) {
            if (!sawFirst) {
                sawFirst = true;
                stats.startupMs = ms(tDec1 - tStart).count();
            }
            decodeSum += decodeMs;
            decodeMax = std::max(decodeMax, decodeMs);
            stats.frames++;
            if (stats.frames % 240 == 0) {
                std::cout << "  [frame " << stats.frames << "] decode "
                          << (1000.0 / (decodeSum / static_cast<double>(stats.frames)))
                          << " fps (software)\n";
            }
            return true;
        }

        if (!sawFirst) {
            sawFirst = true;
            stats.startupMs = ms(tDec1 - tStart).count();
            if (frame->format != AV_PIX_FMT_D3D11) {
                std::cerr << "REX: hardware decode not active (frame format "
                          << av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format))
                          << ", expected d3d11). Use --sw for software benchmark.\n";
                return false;
            }
        }

        // Zero-copy share: decoder texture -> NT handle -> D3D12 resource.
        auto* tex11 = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
        const UINT arraySlice =
            static_cast<UINT>(reinterpret_cast<intptr_t>(frame->data[1]));
        D3D11_TEXTURE2D_DESC desc11 = {};
        tex11->GetDesc(&desc11);

        const auto tShare0 = Clock::now();
        double renderMs = 0.0;
        bool shared = false;
        HRESULT shareHr = E_FAIL;

        // (Re)build the pool on first frame / size change.
        if (static_cast<int>(poolTex.size()) != kBenchPoolSize ||
            poolW != static_cast<int>(desc11.Width) ||
            poolH != static_cast<int>(desc11.Height)) {
            poolTex.clear();
            poolRes12.clear();
            for (HANDLE h2 : poolHandles) {
                if (h2) CloseHandle(h2);
            }
            poolHandles.clear();
            poolTex.resize(kBenchPoolSize);
            poolRes12.resize(kBenchPoolSize);
            poolHandles.resize(kBenchPoolSize, nullptr);
            poolW = static_cast<int>(desc11.Width);
            poolH = static_cast<int>(desc11.Height);
            poolNext = 0;
            for (auto& t : poolTex) {
                D3D11_TEXTURE2D_DESC pd = {};
                pd.Width = static_cast<UINT>(poolW);
                pd.Height = static_cast<UINT>(poolH);
                pd.MipLevels = 1;
                pd.ArraySize = 1;
                pd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                pd.SampleDesc.Count = 1;
                pd.Usage = D3D11_USAGE_DEFAULT;
                pd.BindFlags = D3D11_BIND_RENDER_TARGET |
                               D3D11_BIND_SHADER_RESOURCE;
                pd.MiscFlags = D3D11_RESOURCE_MISC_SHARED |
                               D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
                d3d11->CreateTexture2D(&pd, nullptr, &t);
            }

            // Hardware YUV->RGB video processor (NV12 decode -> BGRA pool).
            vidProc.Reset();
            vidEnum.Reset();
            D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {};
            cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
            cd.InputFrameRate = { 30, 1 };
            cd.InputWidth = static_cast<UINT>(poolW);
            cd.InputHeight = static_cast<UINT>(poolH);
            cd.OutputFrameRate = { 30, 1 };
            cd.OutputWidth = static_cast<UINT>(poolW);
            cd.OutputHeight = static_cast<UINT>(poolH);
            cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
            HRESULT hrVp = vidDev->CreateVideoProcessorEnumerator(&cd, &vidEnum);
            if (SUCCEEDED(hrVp)) {
                hrVp = vidDev->CreateVideoProcessor(vidEnum.Get(), 0, &vidProc);
            }
            if (FAILED(hrVp)) {
                fprintf(stderr, "REX: create video processor failed (0x%08x)\n",
                        static_cast<unsigned>(hrVp));
            }
        }

        // GPU conversion: NV12 decode texture -> BGRA pool slot.
        const int slot = poolNext;
        poolNext = (poolNext + 1) % kBenchPoolSize;
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd = {};
        ivd.FourCC = 0;
        ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        ivd.Texture2D.MipSlice = 0;
        ivd.Texture2D.ArraySlice = arraySlice;
        Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> inView;
        if (!vidProc ||
            FAILED(vidDev->CreateVideoProcessorInputView(
                tex11, vidEnum.Get(), &ivd, &inView))) {
            fprintf(stderr, "REX: create vp input view failed\n");
        }
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd = {};
        ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        ovd.Texture2D.MipSlice = 0;
        Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> outView;
        if (inView && FAILED(vidDev->CreateVideoProcessorOutputView(
                           poolTex[slot].Get(), vidEnum.Get(), &ovd, &outView))) {
            fprintf(stderr, "REX: create vp output view failed\n");
        }
        if (inView && outView) {
            RECT srcRect = { 0, 0, static_cast<LONG>(frame->width),
                             static_cast<LONG>(frame->height) };
            vidCtx->VideoProcessorSetStreamSourceRect(vidProc.Get(), 0, TRUE,
                                                      &srcRect);
            vidCtx1->VideoProcessorSetStreamColorSpace1(
                vidProc.Get(), 0, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
            vidCtx1->VideoProcessorSetOutputColorSpace1(
                vidProc.Get(), DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);

            D3D11_VIDEO_PROCESSOR_STREAM stream = {};
            stream.Enable = TRUE;
            stream.OutputIndex = 0;
            stream.InputFrameOrField = 0;
            stream.pInputSurface = inView.Get();
            const HRESULT hrVp =
                vidCtx->VideoProcessorBlt(vidProc.Get(), outView.Get(), 0, 1,
                                          &stream);
            if (FAILED(hrVp) && stats.shareErrors < 3) {
                fprintf(stderr, "REX: videoprocessor blt failed (0x%08x)\n",
                        static_cast<unsigned>(hrVp));
            }
            fenceValue++;
            slotConsume[slot] = fenceValue;
            d3d11ctx4->Signal(fence11.Get(), fenceValue);
            d3d11Ctx->Flush();
        }
        if (!poolHandles[slot]) {
            Microsoft::WRL::ComPtr<IDXGIResource1> res1;
            shareHr = poolTex[slot]->QueryInterface(IID_PPV_ARGS(&res1));
            if (SUCCEEDED(shareHr)) {
                shareHr = res1->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr,
                                                   &poolHandles[slot]);
            }
            if (FAILED(shareHr)) {
                fprintf(stderr,
                        "REX: createSharedHandle slot=%d failed hr=0x%08x\n",
                        slot, static_cast<unsigned>(shareHr));
            }
        }
        if (poolHandles[slot] && !poolRes12[slot]) {
            shareHr = d3d12.Get()->OpenSharedHandle(
                poolHandles[slot], IID_PPV_ARGS(&poolRes12[slot]));
            if (FAILED(shareHr)) {
                fprintf(stderr,
                        "REX: openSharedHandle slot=%d failed hr=0x%08x\n",
                        slot, static_cast<unsigned>(shareHr));
            }
        }
        if (poolRes12[slot]) {
            ID3D12Resource* tex12 = poolRes12[slot].Get();
            {
                const int heapIdx = static_cast<int>(stats.frames & 1);
                auto* heap = srvHeaps[heapIdx].Get();
                D3D12_CPU_DESCRIPTOR_HANDLE cpu =
                    heap->GetCPUDescriptorHandleForHeapStart();

                D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
                srv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srv.Texture2D.MipLevels = 1;
                d3d12.Get()->CreateShaderResourceView(tex12, &srv, cpu);

                // ── Record + present ──
                const auto tRender0 = Clock::now();
                swap.WaitForFrame();
                queue.Get()->Wait(fence12.Get(), slotConsume[slot]);
                allocator->Reset();
                cmdList->Reset(allocator.Get(), pso.Get());
                ID3D12Resource* back = swap.GetBuffer(swap.GetCurrentIndex());
                D3D12_CPU_DESCRIPTOR_HANDLE rtv = swap.GetRTVHandle(swap.GetCurrentIndex());

                D3D12_RESOURCE_BARRIER barrier = {};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.pResource = back;
                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                cmdList->ResourceBarrier(1, &barrier);
                cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                D3D12_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(winW),
                                      static_cast<float>(winH), 0.0f, 1.0f };
                D3D12_RECT scissor = { 0, 0, winW, winH };
                cmdList->RSSetViewports(1, &vp);
                cmdList->RSSetScissorRects(1, &scissor);

                D3D12_RESOURCE_BARRIER texBarrier = {};
                texBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                texBarrier.Transition.pResource = tex12;
                texBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                texBarrier.Transition.StateAfter =
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                cmdList->ResourceBarrier(1, &texBarrier);

                cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                ID3D12DescriptorHeap* heaps[] = { heap };
                cmdList->SetDescriptorHeaps(1, heaps);
                cmdList->SetGraphicsRootSignature(rootSig.Get());
                cmdList->SetPipelineState(pso.Get());
                cmdList->SetGraphicsRootDescriptorTable(
                    0, heap->GetGPUDescriptorHandleForHeapStart());
                cmdList->DrawInstanced(3, 1, 0, 0);

                D3D12_RESOURCE_BARRIER barrier2 = {};
                barrier2.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier2.Transition.pResource = back;
                barrier2.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                barrier2.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                cmdList->ResourceBarrier(1, &barrier2);
                cmdList->Close();
                queue.Execute(cmdList.Get());
                queue.Get()->Signal(fence12.Get(), ++d12SignalVal);

                swap.Present(1, 0);
                renderMs = ms(Clock::now() - tRender0).count();
                shared = true;
        }
        }
        if (!shared) {
            if (stats.shareErrors < 3) {
                fprintf(stderr,
                        "REX: share failed hr=0x%08x tex=%ux%u fmt=%u misc=0x%x\n",
                        static_cast<unsigned>(shareHr), desc11.Width, desc11.Height,
                        static_cast<unsigned>(desc11.Format),
                        static_cast<unsigned>(desc11.MiscFlags));
            }
            stats.shareErrors++;
            return true;
        }
        const double shareMs = ms(Clock::now() - tShare0).count();
        const double e2eMs = decodeMs + shareMs + renderMs;

        decodeSum += decodeMs;
        decodeMax = std::max(decodeMax, decodeMs);
        shareSum += shareMs;
        shareMax = std::max(shareMax, shareMs);
        renderSum += renderMs;
        renderMax = std::max(renderMax, renderMs);
        e2eSum += e2eMs;
        e2eMax = std::max(e2eMax, e2eMs);
        stats.frames++;

        if (stats.frames % 240 == 0) {
            std::cout << "  [frame " << stats.frames << "] decode "
                      << (1000.0 / (decodeSum / static_cast<double>(stats.frames)))
                      << " fps | share " << shareMs << " ms | render " << renderMs
                      << " ms\n";
        }
        return true;
    };

    bool readEof = false;
    while (!readEof && !fatal) {
        ret = av_read_frame(fmt, pkt);
        if (ret < 0) {
            av_packet_unref(pkt);
            if (ret == AVERROR_EOF) {
                readEof = true;
                avcodec_send_packet(dec, nullptr);  // flush decoder
                break;
            }
            stats.decodeErrors++;
            break;
        }
        if (pkt->stream_index != vIdx) {
            av_packet_unref(pkt);
            continue;
        }
        const auto tDec0 = Clock::now();
        const int sendRet = avcodec_send_packet(dec, pkt);
        av_packet_unref(pkt);
        if (sendRet < 0 && sendRet != AVERROR(EAGAIN)) {
            stats.decodeErrors++;
            continue;
        }
        while (!fatal) {
            const int recvRet = avcodec_receive_frame(dec, frame);
            if (recvRet == AVERROR(EAGAIN) || recvRet == AVERROR_EOF) break;
            if (recvRet < 0) {
                stats.decodeErrors++;
                break;
            }
            fatal = !handleDecodedFrame(tDec0);
            av_frame_unref(frame);
            if (flags.maxFrames > 0 && stats.frames >= flags.maxFrames) break;
        }
        if (flags.maxFrames > 0 && stats.frames >= flags.maxFrames) break;
    }
    if (!fatal && readEof) {
        // Drain frames buffered in the decoder after EOF.
        while (true) {
            const int recvRet = avcodec_receive_frame(dec, frame);
            if (recvRet == AVERROR(EAGAIN) || recvRet == AVERROR_EOF) break;
            if (recvRet < 0) break;
            const auto tFlush = Clock::now();
            fatal = !handleDecodedFrame(tFlush);
            av_frame_unref(frame);
            if (fatal || (flags.maxFrames > 0 && stats.frames >= flags.maxFrames)) break;
        }
    }

    // ── Final measurements ──
    const auto tEnd = Clock::now();
    stats.totalMs = ms(tEnd - tStart).count();
    stats.fps = stats.totalMs > 0.0
                    ? stats.frames * 1000.0 / stats.totalMs
                    : 0.0;
    if (stats.frames > 0) {
        const double n = static_cast<double>(stats.frames);
        stats.decodeAvgMs = decodeSum / n;
        stats.decodeMaxMs = decodeMax;
        stats.shareAvgMs = shareSum / n;
        stats.shareMaxMs = shareMax;
        stats.renderAvgMs = renderSum / n;
        stats.renderMaxMs = renderMax;
        stats.e2eAvgMs = e2eSum / n;
        stats.e2eMaxMs = e2eMax;
    }
    stats.processMemMB = GetProcessPrivateMemoryMB();
    {
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDev;
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
        if (SUCCEEDED(d3d11.As(&dxgiDev)) && SUCCEEDED(dxgiDev->GetAdapter(&adapter)) &&
            SUCCEEDED(adapter.As(&adapter3))) {
            DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
            if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(
                    0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
                stats.gpuMemMB = info.CurrentUsage / (1024.0 * 1024.0);
                stats.gpuBudgetMB = info.Budget / (1024.0 * 1024.0);
            }
        }
    }

    // ── Report ──
    FILE* log = fopen(BuildLogPath(L"benchmark.log").c_str(), "w");
    auto out = [&](const std::string& line) {
        std::cout << line << "\n";
        if (log) fprintf(log, "%s\n", line.c_str());
    };
    out("===== REX BENCHMARK =====");
    out("source:     " + pathUtf8);
    out("input:      " + stats.container + ", " + stats.codec + ", " +
        std::to_string(stats.width) + "x" + std::to_string(stats.height) +
        " @" + std::to_string(stats.containerFps) + " fps");
    out(flags.hwMode == HwMode::SwOnly
            ? "path:       ffmpeg sw decode -> CPU (share/render skipped)"
            : "hw path:    ffmpeg d3d11va decode -> gpu copy -> shared NT handle -> d3d12");
    out("ffmpeg:     " + std::string(av_version_info()));
    if (stats.seekMs > 0.0)
        out("seek:       " + std::to_string(stats.seekMs) + " ms");
    out("startup:    " + std::to_string(stats.startupMs) + " ms (open -> first frame)");
    if (stats.frames > 0 && flags.hwMode != HwMode::SwOnly) {
        out("decode:     avg " + std::to_string(stats.decodeAvgMs) + " ms | max " +
            std::to_string(stats.decodeMaxMs) + " ms | ~" +
            std::to_string(1000.0 / stats.decodeAvgMs) + " fps");
        out("share:      avg " + std::to_string(stats.shareAvgMs) + " ms | max " +
            std::to_string(stats.shareMaxMs) + " ms");
        out("render:     avg " + std::to_string(stats.renderAvgMs) + " ms | max " +
            std::to_string(stats.renderMaxMs) + " ms (vsync paced)");
        out("end-to-end: avg " + std::to_string(stats.e2eAvgMs) + " ms | max " +
            std::to_string(stats.e2eMaxMs) + " ms");
    } else if (stats.frames > 0) {
        out("decode:     avg " + std::to_string(stats.decodeAvgMs) + " ms | max " +
            std::to_string(stats.decodeMaxMs) + " ms | ~" +
            std::to_string(1000.0 / stats.decodeAvgMs) + " fps (software, decode-only)");
    }
    out("frames:     " + std::to_string(stats.frames) + " decoded, " +
        std::to_string(stats.decodeErrors) + " decode errors, " +
        std::to_string(stats.shareErrors) + " share errors");
    out("throughput: " + std::to_string(stats.fps) + " fps over " +
        std::to_string(stats.totalMs / 1000.0) + " s");
    out("gpu vram:   " + std::to_string(stats.gpuMemMB) + " MB used / " +
        std::to_string(stats.gpuBudgetMB) + " MB budget");
    out("process:    " + std::to_string(stats.processMemMB) + " MB private");
    out("log:        " + BuildLogPath(L"benchmark.log"));
    if (fatal) out("status:     ABORTED (hardware decode unavailable)");
    else if (flags.hwMode == HwMode::SwOnly) out("status:     OK (software decode-only)");
    else if (flags.hwMode == HwMode::HwOnly) out("status:     OK (hardware verified)");
    if (log) fclose(log);

    // ── Cleanup ──
    for (HANDLE h : poolHandles) {
        if (h) CloseHandle(h);
    }
    poolHandles.clear();
    av_frame_free(&frame);
    av_packet_free(&pkt);
    av_buffer_unref(&hwDevRef);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    swap.Shutdown();
    queue.Shutdown();
    DestroyWindow(hwnd);
    return fatal ? 1 : 0;
}

int wmain(int argc, wchar_t** argv) {
    // Per-monitor DPI awareness MUST be set before any window creation or
    // display metric query. Without it the render window is sized in
    // virtualized (scaled) pixels inside a DWM-scaled desktop host, so it
    // covers only a fraction of the desktop (e.g. 2/3 on a 150% display).
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    AddVectoredExceptionHandler(1, CrashHandler);
    // SUEF fires only on truly unhandled crashes (after VEH declines).
    // Unlike the VEH it gets all-thread stacks, not just the faulting thread.
    SetUnhandledExceptionFilter(UnhandledCrashFilter);

    std::set_terminate([]() {
        const std::string path = BuildLogPath(L"crash.log");
        FILE* f = nullptr;
        fopen_s(&f, path.c_str(), "a");
        if (f) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            fprintf(f, "\n=== REX TERMINATE %04d-%02d-%02d %02d:%02d:%02d ===\n",
                    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
            if (auto eptr = std::current_exception()) {
                try {
                    std::rethrow_exception(eptr);
                } catch (const std::exception& ex) {
                    fprintf(f, "Uncaught exception: %s\n", ex.what());
                } catch (...) {
                    fprintf(f, "Uncaught exception: unknown type\n");
                }
            } else {
                fprintf(f, "No exception (direct abort/terminate call)\n");
            }
            fprintf(f, "All-thread stacks at terminate:\n");
            DumpAllThreadStacks(f);
            fclose(f);
        }
        std::abort();
    });

    signal(SIGABRT, [](int) {
        const std::string path = BuildLogPath(L"crash.log");
        FILE* f = nullptr;
        fopen_s(&f, path.c_str(), "a");
        if (f) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            fprintf(f, "\n=== REX ABORT %04d-%02d-%02d %02d:%02d:%02d ===\n",
                    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
            fprintf(f, "SIGABRT caught (exe base %p)\n", GetModuleHandleW(nullptr));
            fprintf(f, "All-thread stacks at abort:\n");
            DumpAllThreadStacks(f);
            fclose(f);
        }
        _exit(1);
    });

    bool runtimeMode = false;
    std::wstring file;
    std::string type = "video";
    FillMode fillMode = FillMode::Fill;
    int maxFps = 60;
    bool loop = true;
    bool muted = true;
    float volume = 1.0f;
    float speed = 1.0f;
    bool hardwareAcceleration = true;
    bool forceSdr = false;
    HwMode benchHwMode = HwMode::Auto;
    bool benchmark = false;
    double seekSec = 0.0;
    int maxBenchFrames = 0;

    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        const auto next = [&]() -> std::wstring { return (i + 1 < argc) ? argv[++i] : L""; };
        if (arg == L"--runtime") {
            runtimeMode = true;
        } else if (arg == L"--file" || arg == L"-f") {
            file = next();
        } else if (arg == L"--type" || arg == L"-t") {
            type = WidenToUtf8(next());
        } else if (arg == L"--mode" || arg == L"-m") {
            fillMode = ParseFillMode(WidenToUtf8(next())).value_or(FillMode::Fill);
        } else if (arg == L"--fps") {
            maxFps = std::max(1, _wtoi(next().c_str()));
        } else if (arg == L"--no-loop") {
            loop = false;
        } else if (arg == L"--unmuted") {
            muted = false;
        } else if (arg == L"--volume") {
            volume = _wtof(next().c_str());
        } else if (arg == L"--speed") {
            speed = _wtof(next().c_str());
        } else if (arg == L"--no-hw") {
            hardwareAcceleration = false;
        } else if (arg == L"--force-sdr") {
            forceSdr = true;
        } else if (arg == L"--hw") {
            benchHwMode = HwMode::HwOnly;   // benchmark: abort if HW unavailable
        } else if (arg == L"--sw") {
            benchHwMode = HwMode::SwOnly;   // benchmark: never create D3D11VA ctx
        } else if (arg == L"--benchmark") {
            benchmark = true;
        } else if (arg == L"--seek") {
            seekSec = _wtof(next().c_str());
        } else if (arg == L"--frames") {
            maxBenchFrames = std::max(0, _wtoi(next().c_str()));
        } else if (arg == L"--help" || arg == L"-h") {
            std::cout <<
                "REX Wallpaper CLI\n"
                "Usage: rex-wallpaper-runtime --file <path> [options]\n"
                "  --runtime               persistent mode: read commands from stdin,\n"
                "                          write responses/events to stdout (length-prefixed JSON)\n"
                "  --type <video|image|static> scene type (default: video)\n"
                "  --mode <fill|fit|stretch|center|tile>  fill mode\n"
                "  --fps <n>               max render fps (default: 60)\n"
                "  --no-loop               do not loop\n"
                "  --unmuted               enable audio\n"
                "  --volume <0..1>         playback volume\n"
                "  --speed <0.25..4>       playback speed\n"
                "  --no-hw                 disable hardware acceleration\n"
                "  --force-sdr             force SDR path in shader (diagnostic: skip HDR/PQ)\n"
                "  --hw                    (benchmark) hardware decode ONLY — abort if the\n"
                "                          codec has no D3D11VA hwaccel\n"
                "  --sw                    (benchmark) software decode only — no D3D11VA context\n"
                "  --benchmark             FFmpeg D3D11VA -> D3D12 zero-copy interop benchmark\n"
                "  --seek <sec>            (benchmark) seek to <sec> before decoding\n"
                "  --frames <n>            (benchmark) decode at most <n> frames (0 = whole file)\n";
            return 0;
        }
    }

    if (runtimeMode) {
        return RunRuntime(maxFps, hardwareAcceleration, forceSdr);
    }

    if (benchmark) {
        BenchFlags bf;
        bf.file = file;
        bf.seekSec = seekSec;
        bf.maxFrames = maxBenchFrames;
        bf.hwMode = benchHwMode;
        return RunBenchmark(bf);
    }

    rex::shared::Logger::Instance().Initialize(BuildLogPath(L"cli.log"), rex::shared::LogLevel::Trace);

    if (file.empty()) {
        std::cerr << "REX: --file <path> is required\n";
        return 2;
    }
    if (!rex::wallpaper::SafeExists(file)) {
        std::cerr << "REX: file not found\n";
        return 2;
    }

    if (type == "image") {
        return SetImageWallpaper(file, fillMode);
    }

    auto engine = CreateWallpaperEngine();
    if (!engine) {
        std::cerr << "REX: failed to create wallpaper engine\n";
        return 1;
    }

    WallpaperConfig config;
    config.enableHardwareAcceleration = hardwareAcceleration;
    config.maxFps = maxFps;

    auto result = engine->Initialize(config);
    if (result.IsError()) {
        std::cerr << "REX: engine init failed: " << result.Error().message << "\n";
        return 1;
    }

    // Restore persisted per-monitor assignments (EDID-keyed) from the
    // settings service, if one is available.
    if (auto settings = CreateSettingsService()) {
        engine->SetSettingsService(settings);
    }

    SceneInfo scene;
    scene.id = "cli";
    scene.name = "REX Wallpaper";
    scene.sourcePath = WidenToUtf8(file);
    scene.type = type;
    scene.loop = loop;
    scene.muted = muted;
    scene.volume = volume;
    scene.speed = speed;
    scene.fillMode = fillMode;

    result = engine->LoadScene(scene);
    if (result.IsError()) {
        std::cerr << "REX: scene load failed: " << result.Error().message << "\n";
        engine->Shutdown();
        return 1;
    }

    auto monitors = engine->EnumerateMonitors();
    for (const auto& monitor : monitors) {
        if (monitor.isPrimary) {
            engine->AssignMonitor(monitor.id);
            break;
        }
    }

    result = engine->Play();
    if (result.IsError()) {
        std::cerr << "REX: play failed: " << result.Error().message << "\n";
        engine->Shutdown();
        return 1;
    }

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    engine->Shutdown();
    return 0;
}
