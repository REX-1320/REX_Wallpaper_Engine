# REX Wallpaper - Runtime Source Dump

## src/wallpaper_cli_main.cpp

``cpp
#include <windows.h>

#include <rex/wallpaper/IWallpaperEngine.hpp>
#include <rex/wallpaper/IRendererAdapter.hpp>
#include <shared/Logger.hpp>
#include <shared/Result.hpp>

#include "wallpaper/runtime/Json.hpp"

#include <cstdlib>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <atomic>
#include <mutex>
#include <thread>

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
    int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    std::wstring wide(static_cast<size_t>(size - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wide.data(), size);
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
// Timeline:  T+10s first dump, then every 5s until stop is set.
// Use:       read %TEMP%\REX\crash.log after the freeze to see exactly which
//            thread holds which mutex at the hang point.
static void HangWatchdog(std::atomic<bool>& stop) {
    using namespace std::chrono;
    constexpr int kGraceSecs    = 10;
    constexpr int kIntervalSecs =  5;

    // Wait through grace period in 250ms slices for fast shutdown response.
    for (int i = 0; i < kGraceSecs * 4 && !stop.load(std::memory_order_acquire); ++i)
        std::this_thread::sleep_for(milliseconds(250));

    int dumpCount = 0;
    while (!stop.load(std::memory_order_acquire)) {
        const std::string path = BuildLogPath(L"crash.log");
        FILE* f = nullptr;
        fopen_s(&f, path.c_str(), "a");
        if (f) {
            SYSTEMTIME st;  GetLocalTime(&st);
            fprintf(f,
                "\n=== REX HANG WATCHDOG dump #%d  "
                "%04d-%02d-%02d %02d:%02d:%02d ===\n",
                ++dumpCount,
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
            fprintf(f, "All thread stacks:\n");
            DumpAllThreadStacks(f);
            fprintf(f, "\n--- end of watchdog dump #%d ---\n", dumpCount);
            fclose(f);
        }

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
//            SET_VOLUME, SET_SPEED, SET_LOOP, SET_MONITOR, GET_STATUS,
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

static bool ReadFrame(std::string& out) {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    if (in == INVALID_HANDLE_VALUE || in == nullptr) return false;
    uint32_t len = 0;
    if (!ReadExact(in, &len, 4)) return false;
    if (len == 0 || len > (16u * 1024u * 1024u)) return false;
    out.assign(static_cast<size_t>(len), '\0');
    if (!ReadExact(in, out.data(), len)) return false;
    return true;
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

static constexpr UINT kMaxPreviewSlots = 8;

static struct ClientSurfaceState {
    DWORD pid = 0;
    HANDLE process = nullptr;      // PROCESS_DUP_HANDLE handle to the host
    uint64_t generation = 0;
    UINT slotCount = 0;
    HANDLE fence = nullptr;
    HANDLE slots[kMaxPreviewSlots] = {};
} g_clientSurface;

static void CloseClientSurface() {
    if (g_clientSurface.fence) {
        CloseHandle(g_clientSurface.fence);
        g_clientSurface.fence = nullptr;
    }
    for (UINT i = 0; i < kMaxPreviewSlots; ++i) {
        if (g_clientSurface.slots[i]) {
            CloseHandle(g_clientSurface.slots[i]);
            g_clientSurface.slots[i] = nullptr;
        }
    }
    g_clientSurface.slotCount = 0;
    g_clientSurface.generation = 0;
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
                     "decFps=%.1f rendFps=%.1f | slots=%llu ringBytes=%llu | presents=%llu\n",
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
                     static_cast<unsigned long long>(t.presentCount));
        std::fclose(f);
    }
}


static int RunRuntime(int maxFps, bool hardwareAcceleration, bool forceSdr) {
    // stdout is owned by the frame protocol â€” log to file only.
    rex::shared::Logger::Instance().SetConsoleOutput(false);
    rex::shared::Logger::Instance().Initialize(BuildLogPath(L"runtime.log"), rex::shared::LogLevel::Trace);

    REX_LOG_INFO("Runtime", "Wallpaper runtime started (maxFps={}, hwAccel={})",
                 maxFps, hardwareAcceleration);

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
    while (ReadFrame(frame)) {
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
            CloseClientSurface();
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
            config.maxFps = params["maxFps"].AsInt(config.maxFps);
            config.enableHardwareAcceleration =
                params["hardwareAcceleration"].AsBool(config.enableHardwareAcceleration);
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
            watchdogThread = std::thread(HangWatchdog, std::ref(watchdogStop));

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
            if (!std::filesystem::exists(Utf8ToWiden(path))) {
                WriteResponse(id, false, "file not found: " + path);
                continue;
            }

            FillMode mode = FillMode::Fill;
            if (auto m = ParseFillMode(params["mode"].AsString("fill"))) {
                mode = *m;
            }

            // Images go through the OS wallpaper API â€” no engine needed.
            if (type == "image" || type == "static") {
                // Unload any active engine scene first so a playing video
                // stops decoding and releases its buffers behind the image.
                if (engine) {
                    EngineState st = engine->GetState();
                    if (st == EngineState::Running || st == EngineState::Paused) {
                        auto unloadResult = engine->UnloadScene();
                        if (unloadResult.IsError()) {
                            REX_LOG_WARN("Runtime", "Unload before image load failed: {}",
                                         unloadResult.Error().message.c_str());
                        }
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
            auto result = engine->Play();
            if (cmd == "PLAY") {
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
            cmd == "SET_LOOP" || cmd == "SET_BRIGHTNESS" || cmd == "SET_CONTRAST" ||
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
            CloseClientSurface();
            if (g_clientSurface.process) {
                CloseHandle(g_clientSurface.process);
                g_clientSurface.process = nullptr;
            }
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
                g_clientSurface.generation = s.generation;
                g_clientSurface.slotCount = std::min<UINT>(s.slotCount, kMaxPreviewSlots);
                if (g_clientSurface.process) {
                    DuplicateHandle(GetCurrentProcess(),
                                    reinterpret_cast<HANDLE>(s.fenceHandle),
                                    g_clientSurface.process, &g_clientSurface.fence,
                                    0, FALSE, DUPLICATE_SAME_ACCESS);
                    for (UINT i = 0; i < g_clientSurface.slotCount; ++i) {
                        const HANDLE local = reinterpret_cast<HANDLE>(engine->GetFrameSlotHandle(i));
                        if (!local) continue;
                        DuplicateHandle(GetCurrentProcess(), local,
                                        g_clientSurface.process, &g_clientSurface.slots[i],
                                        0, FALSE, DUPLICATE_SAME_ACCESS);
                    }
                } else {
                    g_clientSurface.fence = reinterpret_cast<HANDLE>(s.fenceHandle);
                    for (UINT i = 0; i < g_clientSurface.slotCount; ++i) {
                        g_clientSurface.slots[i] =
                            reinterpret_cast<HANDLE>(engine->GetFrameSlotHandle(i));
                    }
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
    CloseClientSurface();
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
    if (!std::filesystem::exists(flags.file)) {
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
    UINT64 slotFree[8] = {};      // D3D12 -> D3D11: draw of slot done (wait before blt reuse)
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
                queue.Get()->Signal(fence12.Get(), slotFree[slot] = ++d12SignalVal);

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
    if (!std::filesystem::exists(file)) {
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

``

## src/graphics/renderer/D3D12Renderer.cpp

``cpp

#include "D3D12Renderer.hpp"
#include "wallpaper/decoder/WICTextureLoader.hpp"
#include <shared/Logger.hpp>
#include <d3dcompiler.h>

// DXGI_STATUS_OCCLUDED was introduced in dxgi1_2 / Windows 8 SDK.
#ifndef DXGI_STATUS_OCCLUDED
#define DXGI_STATUS_OCCLUDED 0x087A0001
#endif

namespace rex::wallpaper {

namespace {
    bool IsDeviceRemovedHr(HRESULT hr) {
        return hr == DXGI_ERROR_DEVICE_REMOVED ||
               hr == DXGI_ERROR_DEVICE_RESET ||
               hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
    }
}

// ─── Shader Sources ───────────────────────────────────────────────────────

static const char* g_vertexShaderSource = R"(
struct VSInput {
    float2 pos : POSITION;
    float2 uv : TEXCOORD;
};
struct VSOutput {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD;
};

// Rotation/flip matrix (row-major, root constants b1). Identity by default.
cbuffer TransformCB : register(b1) {
    float4x4 transformMatrix;
};

VSOutput main(VSInput input) {
    VSOutput output;
    output.pos = mul(float4(input.pos, 0.0f, 1.0f), transformMatrix);
    output.uv = input.uv;
    return output;
}
)";

static const char* g_pixelShaderSource = R"(
Texture2D texY : register(t0);
Texture2D texUV : register(t1);
SamplerState sam : register(s0);

cbuffer ColorConstants : register(b0) {
    float4 mode;    // x = matrix (0=BT601, 1=BT709, 2=BT2020)
                    // y = transfer (0=SDR, 1=PQ, 2=HLG)
                    // z = fullRange, w = HDR output
    float4 adj;     // x = brightness, y = contrast, z = saturation, w = paper white nits
    float4 disp;    // x = display peak nits, y = mastering max, z = mastering min, w = MaxCLL
    float4 extra;   // x = inputFormat (0=NV12, 1=RGBA16, 2=P010)
                    // y = debugMode  (0=normal, 1=Y-only, 2=UV-only)
};

struct VSOutput {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD;
};

static const float3x3 BT2020_TO_BT709 = {
    1.6605, -0.1246, -0.0182,
    -0.5876, 1.1323, -0.1006,
    -0.0728, -0.0077, 1.1187
};
static const float3x3 BT709_TO_BT2020 = {
    0.6274, 0.0691, 0.0164,
    0.3293, 0.9195, 0.0880,
    0.0433, 0.0114, 0.8956
};

float3 PqToLinear(float3 e) {
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    float3 p = pow(max(e, 0.0), 1.0 / m2);
    float3 num = max(p - c1, 0.0);
    float3 den = c2 - c3 * p;
    return pow(num / max(den, 1e-9), 1.0 / m1) * 10000.0;
}
float3 LinearToPq(float3 l) {
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    float3 p = pow(max(l / 10000.0, 0.0), m1);
    return pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}

float3 SrgbToLinear(float3 c) {
    float3 lo = c / 12.92;
    float3 hi = pow(max((c + 0.055) / 1.055, 0.0), 2.4);
    return lerp(hi, lo, step(c, float3(0.04045, 0.04045, 0.04045)));
}
float3 LinearToSrgb(float3 c) {
    float3 lo = c * 12.92;
    float3 hi = 1.055 * pow(max(c, 0.0), 1.0 / 2.4) - 0.055;
    return lerp(hi, lo, step(c, float3(0.0031308, 0.0031308, 0.0031308)));
}

float3 AcesFit(float3 x) {
    return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
}

float4 main(VSOutput input) : SV_TARGET {
    const float masterMax = max(disp.y, 1.0);
    const float peak = max(disp.x, 100.0);

    float3 rgb;

    if (extra.x > 1.5) {
        // ── P010 INPUT (GPU-resident HDR) ──────────────────────
        // texY = R16_UNORM (Y plane), texUV = R16G16_UNORM (UV plane).
        // P010 limited-range 10-bit: Y [64,940], Cb/Cr [64,960].
        // R16_UNORM returns the 16-bit value / 65535.0, but P010 stores
        // 10-bit values shifted left by 6 bits. Recover the 10-bit value
        // by dividing by 64 (1023.68 ≈ 65535/64).
        float  yRaw  = texY.Sample(sam, input.uv).r;
        float2 uvRaw = texUV.Sample(sam, input.uv).rg;
        float y10  = yRaw * 1023.68;           // recover 10-bit Y′
        float cb10 = uvRaw.x * 1023.68;        // recover 10-bit Cb
        float cr10 = uvRaw.y * 1023.68;        // recover 10-bit Cr
        float yN  = (y10 - 64.0) / 876.0;      // Y [64,940] → [0,1]
        float cbN = (cb10 - 512.0) / 896.0;    // Cb [64,960] → [-0.5,0.5]
        float crN = (cr10 - 512.0) / 896.0;    // Cr [64,960] → [-0.5,0.5]
        // BT.2020 NCL Y′CbCr → R′G′B′ (on nonlinear PQ values)
        float r = yN + 1.4746 * crN;
        float g = yN - 0.16455 * cbN - 0.57135 * crN;
        float b = yN + 1.8814 * cbN;
        rgb = float3(r, g, b);
        // PQ EOTF → linear BT.2020
        rgb = PqToLinear(saturate(rgb)) / masterMax;
    } else if (mode.y > 0.5) {
        // ── RGBA16 INPUT (software swscale fallback) ───────────
        // swscale converts BT.2020 PQ YCbCr → RGBA64 (PQ-encoded RGB).
        float4 c = texY.Sample(sam, input.uv);
        rgb = PqToLinear(saturate(c.rgb)) / masterMax;
    } else {
        // ── NV12 INPUT (SDR → BT.709 limited-range YUV→R′G′B′) ─
        float  y  = texY.Sample(sam, input.uv).r;
        float2 uv = texUV.Sample(sam, input.uv).rg;
        float3 offset = float3(16.0/255.0, 128.0/255.0, 128.0/255.0);
        float3 scale  = float3(255.0/219.0, 255.0/224.0, 255.0/224.0);
        float3 yuv_norm = (float3(y, uv.x, uv.y) - offset) * scale;
        float r = yuv_norm.x + 1.5748 * yuv_norm.z;
        float g = yuv_norm.x - 0.1873 * yuv_norm.y - 0.4681 * yuv_norm.z;
        float b = yuv_norm.x + 1.8556 * yuv_norm.y;
        rgb = float3(r, g, b);
    }

    // User adjustments (skip for HDR output to preserve range)
    if (mode.w < 0.5) {
        rgb = saturate(rgb * (adj.x * adj.y));
        float luma = dot(rgb, float3(0.2126, 0.7152, 0.0722));
        rgb = lerp(luma, rgb, adj.z);
    }

    // ── DEBUG MODE (bypass output pipeline) ───────────────────────
    if (extra.y > 0.5) {
        if (extra.y < 1.5) {
            // Debug mode 1: Y plane only (grayscale)
            if (extra.x > 1.5) {
                float yRaw = texY.Sample(sam, input.uv).r;
                return float4(yRaw, yRaw, yRaw, 1.0);
            } else {
                float y = texY.Sample(sam, input.uv).r;
                return float4(y, y, y, 1.0);
            }
        } else {
            // Debug mode 2: UV planes only
            if (extra.x > 1.5) {
                float2 uvRaw = texUV.Sample(sam, input.uv).rg;
                return float4(uvRaw.x, uvRaw.y, 0.5, 1.0);
            } else {
                float2 uv = texUV.Sample(sam, input.uv).rg;
                return float4(uv.x, uv.y, 0.5, 1.0);
            }
        }
    }

    // ── OUTPUT ──────────────────────────────────────────────────
    if (mode.w > 0.5) {
        // HDR output (PQ/BT.2020 swapchain): keep BT.2020, PQ-encode.
        float3 lin;
        if (mode.y > 0.5 || extra.x > 1.5) {
            // HDR source: already BT.2020 linear. Scale to display and PQ-encode.
            lin = min(rgb * masterMax * min(1.0, peak / masterMax), peak);
        } else {
            // SDR source: map paper-white to display
            lin = rgb * adj.w;
        }
        return float4(LinearToPq((mode.y > 0.5 || extra.x > 1.5) ? lin : mul(BT709_TO_BT2020, lin)), 1.0);
    }

    // SDR output (sRGB/BT.709 swapchain)
    if (mode.y > 0.5 || extra.x > 1.5) {
        // HDR → SDR: ACES tone-map
        rgb = AcesFit(rgb);
        // Convert BT.2020 → BT.709 for sRGB display
        rgb = mul(BT2020_TO_BT709, rgb);
    }
    return float4(LinearToSrgb(saturate(rgb)), 1.0);
}
)";

// ─── Constructor / Destructor ──────────────────────────────────────────────

D3D12Renderer::~D3D12Renderer() {
    try {
        Shutdown();
    } catch (const std::exception& ex) {
        REX_LOG_ERROR("D3D12", "~D3D12Renderer exception: {}", ex.what());
    } catch (...) {
        REX_LOG_ERROR("D3D12", "~D3D12Renderer unknown exception");
    }
}

// ─── Initialize ───────────────────────────────────────────────────────────

shared::Result<void> D3D12Renderer::Initialize(HWND targetWindow, int width, int height) {
    return Initialize(targetWindow, width, height, nullptr);
}

shared::Result<void> D3D12Renderer::Initialize(HWND targetWindow, int width, int height,
                                               ID3D12Device* adoptDevice) {
    if (m_initialized) {
        return shared::Result<void>(shared::ErrorCode::AlreadyInitialized,
                                    "Renderer already initialized");
    }

    REX_LOG_INFO("D3D12", "Initializing D3D12 renderer ({}x{}{})", width, height,
                 adoptDevice ? " on adopted device" : "");

    m_width  = width;
    m_height = height;

    // ── Device ─────────────────────────────────────────────────────────
    auto devResult = adoptDevice ? m_device.InitializeAdopted(adoptDevice)
                                 : m_device.Initialize();
    if (devResult.IsError()) {
        return devResult;
    }

    // ── Command Queue ──────────────────────────────────────────────────
    auto qResult = m_commandQueue.Initialize(m_device.Get());
    if (qResult.IsError()) {
        return qResult;
    }

    // ── Command allocator + list ───────────────────────────────────────
    m_commandAllocator = m_device.CreateCommandAllocator();
    if (!m_commandAllocator) {
        return shared::Result<void>(shared::ErrorCode::UnknownError,
                                    "Failed to create command allocator");
    }

    // ── Swap Chain ─────────────────────────────────────────────────────
    // --force-sdr: lock swapchain to SDR format before first frame
    if (m_forceSdr) {
        m_swapChain.SetForceSdr(true);
    }
    auto scResult = m_swapChain.Initialize(
        m_commandQueue.Get(), m_device.Get(),
        targetWindow, width, height);
    if (scResult.IsError()) {
        return scResult;
    }

    // ── SRV heap (shader-visible, 64 slots) ───────────────────────────
    m_srvHeap = m_device.CreateDescriptorHeap(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 64,
        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
    if (!m_srvHeap) {
        return shared::Result<void>(shared::ErrorCode::OutOfMemory,
                                    "Failed to create SRV heap");
    }

    // ── Root signature ────────────────────────────────────────────────
    if (!CreateRootSignature()) {
        return shared::Result<void>(shared::ErrorCode::UnknownError,
                                    "Failed to create root signature");
    }

    // ── Vertex buffer ─────────────────────────────────────────────────
    if (!CreateVertexBuffer()) {
        return shared::Result<void>(shared::ErrorCode::UnknownError,
                                    "Failed to create vertex buffer");
    }

    // ── Pipeline state ────────────────────────────────────────────────
    // One PSO per back-buffer format: SDR (8-bit UNORM) and HDR (16-bit
    // float). The shader branches on the HDR flag via root constants.
    if (!CreatePipelineState(DXGI_FORMAT_R8G8B8A8_UNORM, m_pipelineState)) {
        return shared::Result<void>(shared::ErrorCode::UnknownError,
                                    "Failed to create SDR pipeline state");
    }
    m_isHdrOutput = m_swapChain.IsHDR();
    if (m_isHdrOutput) {
        if (!CreatePipelineState(DXGI_FORMAT_R16G16B16A16_FLOAT, m_pipelineStateHdr)) {
            return shared::Result<void>(shared::ErrorCode::UnknownError,
                                        "Failed to create HDR pipeline state");
        }
    }

    // ── Initialize command list ───────────────────────────────────────
    m_commandList = m_device.CreateCommandList(
        m_commandAllocator.Get(), m_pipelineState.Get());
    if (!m_commandList) {
        return shared::Result<void>(shared::ErrorCode::UnknownError,
                                    "Failed to create command list");
    }
    m_commandList->Close();

    // ── Pre-create copy/upload context ring (P5) ──────────────────────
    // Steady-state uploads must not allocate command buffers. Lazy
    // creation inside AcquireCopyContext only fires if these fail below.
    for (UINT i = 0; i < kCopyContextCount; ++i) {
        m_copyContexts[i].allocator = m_device.CreateCommandAllocator();
        if (!m_copyContexts[i].allocator) {
            return shared::Result<void>(shared::ErrorCode::UnknownError,
                                        "Failed to create copy allocator");
        }
        m_copyContexts[i].list =
            m_device.CreateCommandList(m_copyContexts[i].allocator.Get());
        if (!m_copyContexts[i].list) {
            return shared::Result<void>(shared::ErrorCode::UnknownError,
                                        "Failed to create copy list");
        }
        m_copyContexts[i].list->Close(); // created open; Reset() needs closed
    }

    m_initialized = true;
    REX_LOG_INFO("D3D12", "Renderer initialized successfully");
    return shared::Result<void>();
}

void D3D12Renderer::Shutdown() {
    if (!m_initialized) return;

    REX_LOG_INFO("D3D12", "Shutting down renderer...");

    std::lock_guard<std::mutex> lock(m_renderMutex);
    m_commandQueue.WaitForGPU();

    // Release wallpaper-specific resources
    m_texture.Reset();
    m_textureUploadBuffer.Reset();
    m_hasTexture = false;
    for (auto& slot : m_videoSlots) {
        slot.resource.Reset();
        slot.sharedHandle = nullptr;
        slot.srvGpu = {};
    }
    m_videoSlotCount = 0;
    m_activeVideoHandle = nullptr;
    m_hasVideoFrame = false;
    m_activeColorInfo = VideoColorInfo{};
    m_inputFormat = 0.0f;
    m_vertexBuffer.Reset();
    m_pipelineState.Reset();
    m_pipelineStateHdr.Reset();
    m_customPSO.Reset();
    m_customPSOHdr.Reset();
    m_customPsActive = false;
    m_rootSignature.Reset();
    m_srvHeap.Reset();
    m_commandList.Reset();
    m_commandAllocator.Reset();

    // Shutdown shared primitives (order: swap chain → queue → device)
    m_swapChain.Shutdown();
    m_commandQueue.Shutdown();
    // Device has no persistent state to clean up

    m_initialized = false;
    REX_LOG_INFO("D3D12", "Renderer shutdown complete");
}

// ─── Texture Loading ──────────────────────────────────────────────────────

shared::Result<void> D3D12Renderer::LoadTexture(std::string_view path) {
    if (!m_initialized) {
        return shared::Result<void>(shared::ErrorCode::NotInitialized,
                                    "Renderer not initialized");
    }

    REX_LOG_INFO("D3D12", "Loading texture: {}", path);

    TextureData texData = DecodeTextureFromFile(path);
    if (texData.pixels.empty()) {
        return shared::Result<void>(shared::ErrorCode::InvalidFormat,
                                    "WIC decode failed: " + std::string(path));
    }

    std::lock_guard<std::mutex> lock(m_renderMutex);

    if (m_texture) {
        m_commandQueue.WaitForGPU();
        m_texture.Reset();
        m_hasTexture = false;
    }

    if (!UploadTextureToGPU(texData)) {
        return shared::Result<void>(shared::ErrorCode::OutOfMemory,
                                    "GPU texture upload failed");
    }

    REX_LOG_INFO("D3D12", "Texture loaded: {}x{}", texData.width, texData.height);
    return shared::Result<void>();
}

void D3D12Renderer::ClearTexture() {
    std::lock_guard<std::mutex> lock(m_renderMutex);
    if (m_texture) {
        m_commandQueue.WaitForGPU();
        m_texture.Reset();
        m_textureUploadBuffer.Reset();
        m_hasTexture = false;
    }
}

// ─── Video (shared-handle) frames ─────────────────────────────────────────

shared::Result<void> D3D12Renderer::DrawVideoFrame(const VideoFrame& frame,
                                                   ID3D12Fence* waitFence) {
    if (!m_initialized) {
        return shared::Result<void>(shared::ErrorCode::NotInitialized,
                                    "Renderer not initialized");
    }
    if (!frame.valid || !frame.sharedHandle) {
        return shared::Result<void>(shared::ErrorCode::InvalidArgument,
                                    "Invalid video frame");
    }

    // NOTE: no m_renderMutex lock here on purpose — this is called between
    // BeginFrame() and EndFrame(), which already hold the mutex (same
    // convention as UpdateTexturePixels).

    ID3D12Device* dev = m_device.Get();

    // Locate or open the pool slot resource (handles are stable per video
    // open; the decoder's pool has kPoolSize slots, so the cache never
    // needs eviction).
    UINT idx = kMaxVideoSlots;
    for (UINT i = 0; i < m_videoSlotCount; ++i) {
        if (m_videoSlots[i].sharedHandle == frame.sharedHandle) {
            idx = i;
            break;
        }
    }
    if (idx == kMaxVideoSlots) {
        if (m_videoSlotCount >= kMaxVideoSlots) {
            return shared::Result<void>(shared::ErrorCode::InvalidState,
                                        "Video slot cache exhausted");
        }
        idx = m_videoSlotCount;
        HRESULT hr = dev->OpenSharedHandle(frame.sharedHandle,
                                           IID_PPV_ARGS(&m_videoSlots[idx].resource));
        if (FAILED(hr)) {
            if (IsDeviceRemovedHr(hr)) m_deviceRemoved = true;
            return shared::Result<void>(shared::ErrorCode::DeviceLost,
                                        "OpenSharedHandle failed");
        }
        m_videoSlots[idx].sharedHandle = frame.sharedHandle;

        const DXGI_FORMAT fmt = m_videoSlots[idx].resource->GetDesc().Format;
        const D3D12_RESOURCE_DESC resDesc = m_videoSlots[idx].resource->GetDesc();

        // Log full resource descriptor for P010 diagnostics.
        if (fmt == DXGI_FORMAT_P010 || fmt == DXGI_FORMAT_NV12) {
            const char* fmtName = (fmt == DXGI_FORMAT_P010) ? "P010" : "NV12";
            REX_LOG_INFO("D3D12", "shared resource: fmt={} {}x{} "
                         "arraySize={} mipLevels={} sampleCount={} "
                         "dimension={} flags=0x{:x}",
                         fmtName, resDesc.Width, resDesc.Height,
                         resDesc.DepthOrArraySize, resDesc.MipLevels,
                         resDesc.SampleDesc.Count,
                         static_cast<int>(resDesc.Dimension),
                         static_cast<unsigned>(resDesc.Flags));
        }

        // Set shader input format constant for the current frame.
        // 0 = NV12, 1 = RGBA16, 2 = P010
        if (fmt == DXGI_FORMAT_P010) {
            m_inputFormat = 2.0f;
        } else if (fmt == DXGI_FORMAT_NV12) {
            m_inputFormat = 0.0f;
        } else {
            m_inputFormat = 1.0f;  // BGRA / RGBA16F
        }

        const UINT inc = dev->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu =
            m_srvHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE gpu =
            m_srvHeap->GetGPUDescriptorHandleForHeapStart();

        if (fmt == DXGI_FORMAT_NV12 || fmt == DXGI_FORMAT_P010) {
            // NV12/P010: create two SRVs — t0 = Y plane, t1 = UV plane.
            // Descriptor layout: Y at (1 + idx*2), UV at (1 + idx*2 + 1).
            const bool is10bit = (fmt == DXGI_FORMAT_P010);
            const UINT base = 1 + idx * 2;

            D3D12_SHADER_RESOURCE_VIEW_DESC srvY = {};
            srvY.Format = is10bit ? DXGI_FORMAT_R16_UNORM
                                  : DXGI_FORMAT_R8_UNORM;
            srvY.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvY.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvY.Texture2D.MipLevels = 1;
            srvY.Texture2D.PlaneSlice = 0;
            D3D12_CPU_DESCRIPTOR_HANDLE cpuY = cpu;
            cpuY.ptr += base * inc;
            dev->CreateShaderResourceView(m_videoSlots[idx].resource.Get(),
                                          &srvY, cpuY);

            D3D12_SHADER_RESOURCE_VIEW_DESC srvUV = {};
            srvUV.Format = is10bit ? DXGI_FORMAT_R16G16_UNORM
                                   : DXGI_FORMAT_R8G8_UNORM;
            srvUV.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvUV.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvUV.Texture2D.MipLevels = 1;
            srvUV.Texture2D.PlaneSlice = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE cpuUV = cpu;
            cpuUV.ptr += (base + 1) * inc;
            dev->CreateShaderResourceView(m_videoSlots[idx].resource.Get(),
                                          &srvUV, cpuUV);

            // GPU handle points to Y descriptor (UV is next descriptor)
            m_videoSlots[idx].srvGpu = gpu;
            m_videoSlots[idx].srvGpu.ptr += base * inc;
        } else {
            // BGRA / RGBA16F: t0 = main SRV, t1 = dummy (shader samples texUV
            // only in NV12 mode; for RGBA16F the second descriptor must still
            // be valid to satisfy the root signature's 2-descriptor range).
            const UINT base = 1 + idx * 2;

            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = fmt;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            if (fmt == DXGI_FORMAT_B8G8R8A8_UNORM) {
                srvDesc.Shader4ComponentMapping =
                    D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
                        D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2,
                        D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1,
                        D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
                        D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_3);
            } else {
                srvDesc.Shader4ComponentMapping =
                    D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            }
            srvDesc.Texture2D.MipLevels = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE cpuSrv = cpu;
            cpuSrv.ptr += base * inc;
            dev->CreateShaderResourceView(m_videoSlots[idx].resource.Get(),
                                          &srvDesc, cpuSrv);

            // Dummy t1 descriptor (same resource, default mapping)
            D3D12_SHADER_RESOURCE_VIEW_DESC dummy = srvDesc;
            dummy.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            D3D12_CPU_DESCRIPTOR_HANDLE cpuDummy = cpu;
            cpuDummy.ptr += (base + 1) * inc;
            dev->CreateShaderResourceView(m_videoSlots[idx].resource.Get(),
                                          &dummy, cpuDummy);

            m_videoSlots[idx].srvGpu = gpu;
            m_videoSlots[idx].srvGpu.ptr += base * inc;
        }

        m_videoSlotCount++;
    }

    // GPU ordering: the D3D11 video engine blts into this pool slot and
    // signals the shared fence; the D3D12 queue must wait on that signal
    // before the draw in RenderFrame() samples the slot.
    if (waitFence) {
        m_commandQueue.WaitForFence(waitFence, frame.fenceValue);
    }

    m_activeVideoHandle = frame.sharedHandle;
    m_hasVideoFrame = true;
    m_videoWidth  = frame.width;
    m_videoHeight = frame.height;
    m_activeColorInfo = frame.colorInfo;

    // Tone-mapping target: the actual display peak when HDR is active,
    // otherwise SDR reference white (80 nits).
    m_displayPeakNits = m_swapChain.IsHDRActive()
                            ? m_swapChain.GetDisplayMaxNits()
                            : 80.0f;
    return shared::Result<void>();
}

void D3D12Renderer::ClearVideoFrame() {
    std::lock_guard<std::mutex> lock(m_renderMutex);

    m_commandQueue.WaitForGPU();
    for (auto& slot : m_videoSlots) {
        slot.resource.Reset();
        slot.sharedHandle = nullptr;
        slot.srvGpu = {};
    }
    m_videoSlotCount = 0;
    m_activeVideoHandle = nullptr;
    m_hasVideoFrame = false;
    m_videoWidth  = 0;
    m_videoHeight = 0;
    m_activeColorInfo = VideoColorInfo{};
    m_inputFormat = 0.0f;
}

// ─── Update Texture Pixels (video frames) ────────────────────────────────

shared::Result<void> D3D12Renderer::UpdateTexturePixels(
    const uint8_t* pixels, int width, int height, int rowPitch) {

    if (!m_initialized) {
        return shared::Result<void>(shared::ErrorCode::NotInitialized,
                                    "Renderer not initialized");
    }
    if (!pixels || width <= 0 || height <= 0) {
        return shared::Result<void>(shared::ErrorCode::InvalidArgument,
                                    "Invalid pixel data");
    }

    // NOTE: no m_renderMutex lock here on purpose. The only caller
    // (Engine::RenderLoop) invokes this between BeginFrame() and
    // EndFrame(), which already hold the mutex for the whole frame.
    // Recursive locking on the render thread would throw
    // std::system_error (MSVC detects the re-lock) and abort the process.
    ID3D12Device* dev = m_device.Get();

    if (!m_texture || m_texWidth != width || m_texHeight != height) {
        if (m_texture) {
            m_commandQueue.WaitForGPU();
            m_texture.Reset();
            m_textureUploadBuffer.Reset();
        }
        m_textureRecreations++;

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width            = static_cast<UINT64>(width);
        texDesc.Height           = static_cast<UINT>(height);
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels        = 1;
        texDesc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        m_texture = m_device.CreateCommittedResource(
            heapProps, D3D12_HEAP_FLAG_NONE, texDesc, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!m_texture) {
            if (IsDeviceRemovedHr(m_device.GetDeviceRemovedReason())) m_deviceRemoved = true;
            return shared::Result<void>(shared::ErrorCode::OutOfMemory,
                                        "Failed to create video texture");
        }

        // Row pitch of a buffer -> texture copy must be aligned to
        // D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (256). Decoder row pitches
        // (e.g. 2706*4 = 10824) rarely are, so pad each row in the
        // upload buffer and use the aligned pitch in the footprint.
        const UINT64 alignedPitch =
            (static_cast<UINT64>(rowPitch) + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
            & ~(static_cast<UINT64>(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) - 1);

        UINT64 uploadSize = alignedPitch * height;
        uploadSize = (uploadSize + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1)
                   & ~(static_cast<UINT64>(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT) - 1);

        D3D12_RESOURCE_DESC uploadDesc = {};
        uploadDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        uploadDesc.Width            = uploadSize;
        uploadDesc.Height           = 1;
        uploadDesc.DepthOrArraySize = 1;
        uploadDesc.MipLevels        = 1;
        uploadDesc.Format           = DXGI_FORMAT_UNKNOWN;
        uploadDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        uploadDesc.SampleDesc.Count = 1;

        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        m_textureUploadBuffer = m_device.CreateCommittedResource(
            heapProps, D3D12_HEAP_FLAG_NONE, uploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!m_textureUploadBuffer) {
            if (IsDeviceRemovedHr(m_device.GetDeviceRemovedReason())) m_deviceRemoved = true;
            m_texture.Reset();
            return shared::Result<void>(shared::ErrorCode::OutOfMemory,
                                        "Failed to create video upload buffer");
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels     = 1;

        D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
        dev->CreateShaderResourceView(m_texture.Get(), &srvDesc, cpu);
        m_textureSRVGpuHandle = m_srvHeap->GetGPUDescriptorHandleForHeapStart();

        m_texWidth  = width;
        m_texHeight = height;
        m_hasTexture = true;

        m_textureBytes = static_cast<uint64_t>(width) * height * 4;
        m_uploadBytes  = uploadSize;
    }

    // Copy pixels to upload buffer
    void* mapped = nullptr;
    HRESULT hr = m_textureUploadBuffer->Map(0, nullptr, &mapped);
    if (FAILED(hr)) {
        if (IsDeviceRemovedHr(hr)) m_deviceRemoved = true;
        return shared::Result<void>(shared::ErrorCode::UnknownError, "Map failed");
    }

    {
        const uint8_t* src = pixels;
        uint8_t* dst = static_cast<uint8_t*>(mapped);
        const UINT64 alignedPitch =
            (static_cast<UINT64>(rowPitch) + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
            & ~(static_cast<UINT64>(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) - 1);
        for (int row = 0; row < height; ++row) {
            memcpy(dst, src, static_cast<size_t>(rowPitch));
            src += rowPitch;
            dst += alignedPitch;
        }
    }
    m_textureUploadBuffer->Unmap(0, nullptr);

    // Persistent context ring (P5) — no per-upload allocation.
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> copyAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> copyList;
    if (auto ctxRes = AcquireCopyContext(copyAllocator, copyList); ctxRes.IsError()) {
        return ctxRes;
    }
    copyList->Reset(copyAllocator.Get(), nullptr);

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource        = m_texture.Get();
    dstLoc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = m_textureUploadBuffer.Get();
    srcLoc.Type      = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint.Offset = 0;
    srcLoc.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
    srcLoc.PlacedFootprint.Footprint.Width    = static_cast<UINT>(width);
    srcLoc.PlacedFootprint.Footprint.Height   = static_cast<UINT>(height);
    srcLoc.PlacedFootprint.Footprint.Depth    = 1;
    srcLoc.PlacedFootprint.Footprint.RowPitch =
        static_cast<UINT>((static_cast<UINT64>(rowPitch)
            + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
            & ~(static_cast<UINT64>(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) - 1));

    copyList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = m_texture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    copyList->ResourceBarrier(1, &barrier);

    copyList->Close();
    m_commandQueue.Execute(copyList.Get());
    m_commandQueue.WaitForGPU();

    m_uploads++;

    return shared::Result<void>();
}

// ─── Frame Rendering ──────────────────────────────────────────────────────

// P5: persistent copy-context ring. After warm-up this never allocates;
// the counter only moves if a context had to be (re)created during an
// upload — steady-state growth there is a regression signal.
shared::Result<void> D3D12Renderer::AcquireCopyContext(
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>& allocatorOut,
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>& listOut) {
    CopyContext& ctx = m_copyContexts[m_copyContextNext];
    m_copyContextNext = (m_copyContextNext + 1) % kCopyContextCount;

    if (!ctx.allocator || !ctx.list) {
        m_allocatorsPerUpload.fetch_add(1, std::memory_order_relaxed);
        ctx.allocator = m_device.CreateCommandAllocator();
        if (!ctx.allocator) {
            if (IsDeviceRemovedHr(m_device.GetDeviceRemovedReason())) {
                m_deviceRemoved = true;
            }
            return shared::Result<void>(shared::ErrorCode::UnknownError,
                                        "CreateCommandAllocator failed");
        }
        ctx.list = m_device.CreateCommandList(ctx.allocator.Get());
        if (!ctx.list) {
            if (IsDeviceRemovedHr(m_device.GetDeviceRemovedReason())) {
                m_deviceRemoved = true;
            }
            return shared::Result<void>(shared::ErrorCode::UnknownError,
                                        "CreateCommandList failed");
        }
        ctx.list->Close();
    }

    allocatorOut = ctx.allocator;
    listOut      = ctx.list;
    return shared::Result<void>();
}

void D3D12Renderer::BeginFrame() {
    m_renderMutex.lock();

    m_swapChain.WaitForFrame();

    m_commandAllocator->Reset();
    m_commandList->Reset(m_commandAllocator.Get(), m_pipelineState.Get());
}

void D3D12Renderer::RenderFrame() {
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_swapChain.GetRTVHandle(
        m_swapChain.GetCurrentIndex());
    ID3D12Resource* backBuffer = m_swapChain.GetBuffer(
        m_swapChain.GetCurrentIndex());

    // Transition back buffer to render target
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = backBuffer;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &barrier);

    m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
    m_commandList->ClearRenderTargetView(rtvHandle, m_clearColor, 0, nullptr);

    // Viewport with fill mode
    D3D12_VIEWPORT viewport;
    D3D12_RECT scissor;
    ComputeFillViewport(viewport, scissor);
    m_commandList->RSSetViewports(1, &viewport);
    m_commandList->RSSetScissorRects(1, &scissor);

    // Draw textured fullscreen quad (video frame takes priority over the
    // static texture).
    ID3D12Resource* texToDraw = nullptr;
    D3D12_GPU_DESCRIPTOR_HANDLE srvToDraw{};
    bool videoActive = false;
    if (m_hasVideoFrame && m_activeVideoHandle) {
        for (UINT i = 0; i < m_videoSlotCount; ++i) {
            if (m_videoSlots[i].sharedHandle == m_activeVideoHandle) {
                texToDraw = m_videoSlots[i].resource.Get();
                srvToDraw = m_videoSlots[i].srvGpu;
                videoActive = true;
                break;
            }
        }
    } else if (m_hasTexture) {
        texToDraw = m_texture.Get();
        srvToDraw = m_textureSRVGpuHandle;
    }

    if (texToDraw) {
        // Shared textures arrive in D3D12_RESOURCE_STATE_COMMON; transition
        // for sampling, then return to COMMON so the D3D11 video engine can
        // blt into the slot again.
        if (videoActive) {
            barrier.Transition.pResource   = texToDraw;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            m_commandList->ResourceBarrier(1, &barrier);
        }

        m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());
        ID3D12PipelineState* pso = nullptr;
        // --force-sdr: always use SDR PSO (R8G8B8A8_UNORM) even if swap chain
        // is R16F, because our NV12 shader outputs sRGB, not PQ.
        const bool useHdrPso = m_forceSdr ? false : m_isHdrOutput;
        if (m_customPsActive) {
            pso = useHdrPso ? m_customPSOHdr.Get() : m_customPSO.Get();
        }
        if (!pso) {
            pso = useHdrPso ? m_pipelineStateHdr.Get()
                            : m_pipelineState.Get();
        }
        m_commandList->SetPipelineState(pso);
        m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

        D3D12_VERTEX_BUFFER_VIEW vbv = {};
        vbv.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress();
        vbv.SizeInBytes    = sizeof(Vertex) * 4;
        vbv.StrideInBytes  = sizeof(Vertex);
        m_commandList->IASetVertexBuffers(0, 1, &vbv);

        ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
        m_commandList->SetDescriptorHeaps(1, heaps);
        m_commandList->SetGraphicsRootDescriptorTable(0, srvToDraw);

        // Color constants: decode/transfer, adjustments, HDR metadata.
        float cb[16] = {};
        BuildColorConstants(cb, m_isHdrOutput);
        m_commandList->SetGraphicsRoot32BitConstants(1, 16, cb, 0);

        // Transform matrix (rotation/flip) for the vertex shader.
        m_commandList->SetGraphicsRoot32BitConstants(2, 16,
                                                     &m_transformMatrix, 0);

        m_commandList->DrawInstanced(4, 1, 0, 0);

        if (videoActive) {
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
            m_commandList->ResourceBarrier(1, &barrier);
        }
    }

    // Transition back to present
    barrier.Transition.pResource   = backBuffer;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    m_commandList->ResourceBarrier(1, &barrier);
}

void D3D12Renderer::EndFrame() {
    m_commandList->Close();
    m_commandQueue.Execute(m_commandList.Get());

    HRESULT hr = m_swapChain.Present(1, 0);

    // ── Present diagnostics ──────────────────────────────────────────────
    //
    // DXGI_STATUS_OCCLUDED (0x087A0001) is a SUCCESS code returned when
    // DWM drops the frame because it considers the window fully occluded.
    // FAILED() and hr < 0 checks will NOT catch it — it is semantically
    // "success but no-op."

    if (FAILED(hr)) {
        REX_LOG_ERROR("D3D12", "Present FAILED (0x{:08x})",
                     static_cast<unsigned long>(hr));

        if (hr == DXGI_ERROR_DEVICE_REMOVED) {
            HRESULT drr = m_device.GetDeviceRemovedReason();
            REX_LOG_ERROR("D3D12", "Device removed, reason 0x{:08x}",
                         static_cast<unsigned long>(drr));
            m_deviceRemoved = true;
        } else if (hr == DXGI_ERROR_DEVICE_RESET) {
            REX_LOG_WARN("D3D12", "Device reset — will attempt recovery");
            m_deviceRemoved = true;
        } else if (IsDeviceRemovedHr(hr)) {
            m_deviceRemoved = true;
        }
    } else if (hr == DXGI_STATUS_OCCLUDED) {
        // DWM dropped this frame. Throttle to ~1 log every 10 seconds
        // so the log isn't spammed at 60 fps.
        static thread_local int s_occludedCount = 0;
        s_occludedCount++;
        if (s_occludedCount % 300 == 1) {
            REX_LOG_WARN("D3D12",
                "Present returned DXGI_STATUS_OCCLUDED. "
                "DWM considers the render target fully occluded "
                "(frame count: {}).", s_occludedCount);
        }
    } else if (hr != S_OK) {
        REX_LOG_INFO("D3D12", "Present returned non-S_OK: 0x{:08x}",
                    static_cast<unsigned long>(hr));
    } else {
        static thread_local int s_presentCount = 0;
        s_presentCount++;
        if (s_presentCount == 1 || s_presentCount % 600 == 0) {
            REX_LOG_INFO("D3D12", "Present OK (frame count: {})", s_presentCount);
        }
    }

    m_commandQueue.Signal();
    m_renderMutex.unlock();
}

void D3D12Renderer::SetClearColor(float r, float g, float b) {
    m_clearColor[0] = r;
    m_clearColor[1] = g;
    m_clearColor[2] = b;
    m_clearColor[3] = 1.0f;
}

void D3D12Renderer::Resize(int width, int height) {
    if (width == m_width && height == m_height) return;

    std::lock_guard<std::mutex> lock(m_renderMutex);

    REX_LOG_INFO("D3D12", "Resizing to {}x{}", width, height);

    m_commandQueue.WaitForGPU();
    m_swapChain.Resize(m_device.Get(), width, height);

    m_width  = width;
    m_height = height;
}

// ─── Upload Texture to GPU (static) ───────────────────────────────────────

bool D3D12Renderer::UploadTextureToGPU(const TextureData& data) {
    // Destination texture in DEFAULT heap
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width            = static_cast<UINT64>(data.width);
    texDesc.Height           = static_cast<UINT>(data.height);
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels        = 1;
    texDesc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    m_texture = m_device.CreateCommittedResource(
        heapProps, D3D12_HEAP_FLAG_NONE, texDesc, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!m_texture) {
        REX_LOG_ERROR("D3D12", "CreateCommittedResource (texture) failed");
        if (IsDeviceRemovedHr(m_device.GetDeviceRemovedReason())) m_deviceRemoved = true;
        return false;
    }

    // Upload buffer
    UINT64 uploadSize = static_cast<UINT64>(data.rowPitch) * data.height;
    uploadSize = (uploadSize + 511) & ~511ULL;

    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC uploadDesc = {};
    uploadDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width            = uploadSize;
    uploadDesc.Height           = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels        = 1;
    uploadDesc.Format           = DXGI_FORMAT_UNKNOWN;
    uploadDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    uploadDesc.SampleDesc.Count = 1;

    auto uploadBuffer = m_device.CreateCommittedResource(
        heapProps, D3D12_HEAP_FLAG_NONE, uploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!uploadBuffer) {
        REX_LOG_ERROR("D3D12", "CreateCommittedResource (upload) failed");
        if (IsDeviceRemovedHr(m_device.GetDeviceRemovedReason())) m_deviceRemoved = true;
        m_texture.Reset();
        return false;
    }

    // Copy pixel data
    void* mapped = nullptr;
    HRESULT hr = uploadBuffer->Map(0, nullptr, &mapped);
    if (FAILED(hr)) {
        if (IsDeviceRemovedHr(hr)) m_deviceRemoved = true;
        m_texture.Reset();
        return false;
    }
    {
        const uint8_t* src = data.pixels.data();
        uint8_t* dst = static_cast<uint8_t*>(mapped);
        for (int row = 0; row < data.height; ++row) {
            memcpy(dst, src, static_cast<size_t>(data.rowPitch));
            src += data.rowPitch;
            dst += data.rowPitch;
        }
    }
    uploadBuffer->Unmap(0, nullptr);

    // Copy via persistent context ring (P5) — no per-upload allocation.
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> uploadAlloc;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> uploadList;
    if (auto ctxRes = AcquireCopyContext(uploadAlloc, uploadList); ctxRes.IsError()) {
        m_texture.Reset();
        return false;
    }
    uploadList->Reset(uploadAlloc.Get(), nullptr);

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource        = m_texture.Get();
    dstLoc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = uploadBuffer.Get();
    srcLoc.Type      = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint.Offset = 0;
    srcLoc.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
    srcLoc.PlacedFootprint.Footprint.Width    = static_cast<UINT>(data.width);
    srcLoc.PlacedFootprint.Footprint.Height   = static_cast<UINT>(data.height);
    srcLoc.PlacedFootprint.Footprint.Depth    = 1;
    srcLoc.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(data.rowPitch);

    uploadList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = m_texture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    uploadList->ResourceBarrier(1, &barrier);

    uploadList->Close();
    m_commandQueue.Execute(uploadList.Get());
    m_commandQueue.WaitForGPU();

    // Create SRV
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels     = 1;

    D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    m_device.Get()->CreateShaderResourceView(m_texture.Get(), &srvDesc, cpu);
    m_textureSRVGpuHandle = m_srvHeap->GetGPUDescriptorHandleForHeapStart();

    m_hasTexture = true;
    m_texWidth   = data.width;
    m_texHeight  = data.height;

    return true;
}

// ─── Pipeline State ──────────────────────────────────────────────────────

// ─── Transform (rotation / flip) ──────────────────────────────────────────

void D3D12Renderer::SetRotation(float degrees) {
    std::lock_guard<std::mutex> lock(m_renderMutex);
    m_rotation = degrees;
    UpdateTransformMatrix();
}

void D3D12Renderer::SetFlip(bool horizontal, bool vertical) {
    std::lock_guard<std::mutex> lock(m_renderMutex);
    m_flipH = horizontal;
    m_flipV = vertical;
    UpdateTransformMatrix();
}

void D3D12Renderer::UpdateTransformMatrix() {
    using namespace DirectX;
    XMMATRIX rot = XMMatrixRotationZ(XMConvertToRadians(m_rotation));
    XMMATRIX flip = XMMatrixScaling(m_flipH ? -1.0f : 1.0f,
                                     m_flipV ? -1.0f : 1.0f, 1.0f);
    // Row-major: the vertex shader uses row-vector mul(v, M), so the
    // stored matrix is consumed without a transpose.
    XMStoreFloat4x4(&m_transformMatrix, flip * rot);
}

// ─── Custom pixel shader hook ─────────────────────────────────────────────

bool D3D12Renderer::SetCustomPixelShader(const std::string& hlslSource) {
    if (!m_initialized) {
        REX_LOG_ERROR("D3D12", "Custom PS: renderer not initialized");
        return false;
    }

    std::lock_guard<std::mutex> lock(m_renderMutex);

    Microsoft::WRL::ComPtr<ID3DBlob> psBlob, errorBlob;
    HRESULT hr = D3DCompile(hlslSource.c_str(), hlslSource.size(), nullptr,
                            nullptr, nullptr, "main", "ps_5_0",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                            &psBlob, &errorBlob);
    if (FAILED(hr)) {
        std::string msg = errorBlob
            ? std::string(static_cast<const char*>(errorBlob->GetBufferPointer()),
                          errorBlob->GetBufferSize())
            : "unknown compile error";
        REX_LOG_ERROR("D3D12", "Custom PS compile failed: {}", msg);
        return false;
    }

    // Mirror the default pipeline setup: SDR always, HDR when the swap
    // chain is an R16F target. The root signature is shared, so the custom
    // shader must bind the same t0 texture / s0 sampler / b0 constants.
    Microsoft::WRL::ComPtr<ID3D12PipelineState> customSdr, customHdr;
    if (!CreatePipelineStateWithPs(DXGI_FORMAT_R8G8B8A8_UNORM,
                                   psBlob->GetBufferPointer(),
                                   psBlob->GetBufferSize(), customSdr)) {
        REX_LOG_ERROR("D3D12", "Custom PS: SDR PSO creation failed");
        return false;
    }
    if (m_isHdrOutput) {
        if (!CreatePipelineStateWithPs(DXGI_FORMAT_R16G16B16A16_FLOAT,
                                       psBlob->GetBufferPointer(),
                                       psBlob->GetBufferSize(), customHdr)) {
            REX_LOG_ERROR("D3D12", "Custom PS: HDR PSO creation failed");
            return false;
        }
    }

    m_customPSO = customSdr;
    m_customPSOHdr = customHdr;
    m_customPsSource = hlslSource;
    m_customPsActive = true;
    REX_LOG_INFO("D3D12", "Custom pixel shader installed");
    return true;
}

void D3D12Renderer::ClearCustomPixelShader() {
    std::lock_guard<std::mutex> lock(m_renderMutex);
    m_customPSO.Reset();
    m_customPSOHdr.Reset();
    m_customPsSource.clear();
    m_customPsActive = false;
    REX_LOG_INFO("D3D12", "Custom pixel shader reverted to default");
}

bool D3D12Renderer::CreateRootSignature() {
    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType             = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors        = 2;   // t0=Y plane, t1=UV plane (NV12)
    range.BaseShaderRegister    = 0;
    range.RegisterSpace         = 0;

    D3D12_ROOT_PARAMETER params[3] = {};

    // t0: texture (static or video slot)
    params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges   = &range;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // b0: color constants (mode / adjustments / display metadata) —
    // 16 floats, updated per frame via SetGraphicsRoot32BitConstants.
    params[1].ParameterType                    = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister         = 0;
    params[1].Constants.RegisterSpace          = 0;
    params[1].Constants.Num32BitValues         = 16;
    params[1].ShaderVisibility                 = D3D12_SHADER_VISIBILITY_PIXEL;

    // b1: transform matrix (rotation/flip) — 16 floats, vertex shader.
    params[2].ParameterType                    = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants.ShaderRegister         = 1;
    params[2].Constants.RegisterSpace          = 0;
    params[2].Constants.Num32BitValues         = 16;
    params[2].ShaderVisibility                 = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter         = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD         = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters     = 3;
    desc.pParameters       = params;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers   = &sampler;
    desc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    Microsoft::WRL::ComPtr<ID3DBlob> sigBlob, errorBlob;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                              &sigBlob, &errorBlob);
    if (FAILED(hr)) {
        REX_LOG_ERROR("D3D12", "SerializeRootSignature failed: {:08x}", hr);
        return false;
    }

    m_rootSignature = m_device.CreateRootSignature(
        sigBlob->GetBufferPointer(), sigBlob->GetBufferSize());
    if (!m_rootSignature) return false;

    return true;
}

bool D3D12Renderer::CreatePipelineState(
    DXGI_FORMAT rtvFormat,
    Microsoft::WRL::ComPtr<ID3D12PipelineState>& outPs) {
    Microsoft::WRL::ComPtr<ID3DBlob> psBlob, errorBlob;

    HRESULT hr = D3DCompile(g_pixelShaderSource, strlen(g_pixelShaderSource),
           nullptr, nullptr, nullptr, "main", "ps_5_0",
           D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psBlob, &errorBlob);
    if (FAILED(hr)) {
        REX_LOG_ERROR("D3D12", "Pixel shader compile failed");
        return false;
    }

    return CreatePipelineStateWithPs(rtvFormat,
                                     psBlob->GetBufferPointer(),
                                     psBlob->GetBufferSize(), outPs);
}

bool D3D12Renderer::CreatePipelineStateWithPs(
    DXGI_FORMAT rtvFormat,
    const void* psBytecode, SIZE_T psSize,
    Microsoft::WRL::ComPtr<ID3D12PipelineState>& outPs) {
    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, errorBlob;

    HRESULT hr = D3DCompile(g_vertexShaderSource, strlen(g_vertexShaderSource),
                nullptr, nullptr, nullptr, "main", "vs_5_0",
                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsBlob, &errorBlob);
    if (FAILED(hr)) {
        REX_LOG_ERROR("D3D12", "Vertex shader compile failed");
        return false;
    }

    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature            = m_rootSignature.Get();
    psoDesc.VS                        = {vsBlob->GetBufferPointer(), vsBlob->GetBufferSize()};
    psoDesc.PS                        = {psBytecode, psSize};
    psoDesc.InputLayout               = {inputLayout, 2};
    psoDesc.RasterizerState.FillMode  = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode  = D3D12_CULL_MODE_NONE;
    psoDesc.BlendState.RenderTarget[0].BlendEnable       = FALSE;
    psoDesc.BlendState.RenderTarget[0].SrcBlend          = D3D12_BLEND_ONE;
    psoDesc.BlendState.RenderTarget[0].DestBlend         = D3D12_BLEND_ZERO;
    psoDesc.BlendState.RenderTarget[0].BlendOp           = D3D12_BLEND_OP_ADD;
    psoDesc.BlendState.RenderTarget[0].SrcBlendAlpha     = D3D12_BLEND_ONE;
    psoDesc.BlendState.RenderTarget[0].DestBlendAlpha    = D3D12_BLEND_ZERO;
    psoDesc.BlendState.RenderTarget[0].BlendOpAlpha      = D3D12_BLEND_OP_ADD;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.SampleMask              = UINT_MAX;
    psoDesc.PrimitiveTopologyType   = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets        = 1;
    psoDesc.RTVFormats[0]           = rtvFormat;
    psoDesc.SampleDesc.Count        = 1;

    outPs = m_device.CreateGraphicsPipelineState(psoDesc);
    if (!outPs) {
        REX_LOG_ERROR("D3D12", "CreateGraphicsPipelineState failed (format {:04x})",
                      static_cast<unsigned int>(rtvFormat));
        return false;
    }

    return true;
}

// ─── Color Pipeline ──────────────────────────────────────────────────────

void D3D12Renderer::BuildColorConstants(float cb[16], bool hdrOutput) const {
    const VideoColorInfo& ci = m_activeColorInfo;

    // mode: matrix | transfer | fullRange | isHdrOutput
    cb[0] = static_cast<float>(ci.yuvMatrix);
    cb[1] = static_cast<float>(ci.transfer);
    cb[2] = ci.fullRange ? 1.0f : 0.0f;
    // --force-sdr overrides hdrOutput to 0 (SDR path in shader)
    cb[3] = (m_forceSdr || !hdrOutput) ? 0.0f : 1.0f;

    // adj: brightness | contrast | saturation | paper white
    cb[4] = m_brightness;                 // brightness (identity 1.0)
    cb[5] = m_contrast;                   // contrast (identity 1.0)
    cb[6] = m_saturation;                 // saturation (identity 1.0)
    cb[7] = 203.0f;                    // SDR reference white on HDR (nits)

    // disp: display peak | mastering max | mastering min | MaxCLL
    cb[8] = m_displayPeakNits;           // tone-mapping target (nits)
    cb[9] = ci.masteringDisplayMaxLuminance > 0.0f
                ? ci.masteringDisplayMaxLuminance : 1000.0f;
    cb[10] = ci.masteringDisplayMinLuminance;
    cb[11] = ci.maxContentLightLevel;

    // extra: inputFormat (0=NV12, 1=RGBA16, 2=P010), debugMode
    cb[12] = m_inputFormat;
    cb[13] = m_debugMode;
    cb[14] = cb[15] = 0.0f;
}

bool D3D12Renderer::CreateVertexBuffer() {
    Vertex vertices[] = {
        {-1.0f, -1.0f, 0.0f, 1.0f},
        { 1.0f, -1.0f, 1.0f, 1.0f},
        {-1.0f,  1.0f, 0.0f, 0.0f},
        { 1.0f,  1.0f, 1.0f, 0.0f}
    };

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC resDesc = {};
    resDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    resDesc.Width            = sizeof(vertices);
    resDesc.Height           = 1;
    resDesc.DepthOrArraySize = 1;
    resDesc.MipLevels        = 1;
    resDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resDesc.SampleDesc.Count = 1;

    m_vertexBuffer = m_device.CreateCommittedResource(
        heapProps, D3D12_HEAP_FLAG_NONE, resDesc, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!m_vertexBuffer) return false;

    void* mapped = nullptr;
    m_vertexBuffer->Map(0, nullptr, &mapped);
    memcpy(mapped, vertices, sizeof(vertices));
    m_vertexBuffer->Unmap(0, nullptr);

    return true;
}

// ─── Fill Mode Viewport ──────────────────────────────────────────────────

void D3D12Renderer::ComputeFillViewport(D3D12_VIEWPORT& vp, D3D12_RECT& scissor) const {
    float screenW = static_cast<float>(m_width);
    float screenH = static_cast<float>(m_height);

    vp.TopLeftX = 0.0f; vp.TopLeftY = 0.0f;
    vp.Width = screenW; vp.Height = screenH;
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;

    scissor.left = 0; scissor.top = 0;
    scissor.right = m_width; scissor.bottom = m_height;

    if (!m_hasTexture && !m_hasVideoFrame) return;

    const int texW = m_hasVideoFrame ? m_videoWidth  : m_texWidth;
    const int texH = m_hasVideoFrame ? m_videoHeight : m_texHeight;
    if (texW <= 0 || texH <= 0) return;

    float texAspect = static_cast<float>(texW) / static_cast<float>(texH);
    float screenAspect = screenW / screenH;

    switch (m_fillMode) {
        case FillMode::Stretch:
            break;
        case FillMode::Fit: {
            float fitW, fitH;
            if (texAspect > screenAspect) {
                fitW = screenW; fitH = screenW / texAspect;
            } else {
                fitH = screenH; fitW = screenH * texAspect;
            }
            vp.TopLeftX = (screenW - fitW) * 0.5f;
            vp.TopLeftY = (screenH - fitH) * 0.5f;
            vp.Width = fitW; vp.Height = fitH;
            break;
        }
        case FillMode::Fill: {
            float fillW, fillH;
            if (texAspect > screenAspect) {
                fillH = screenH; fillW = screenH * texAspect;
            } else {
                fillW = screenW; fillH = screenW / texAspect;
            }
            vp.TopLeftX = (screenW - fillW) * 0.5f;
            vp.TopLeftY = (screenH - fillH) * 0.5f;
            vp.Width = fillW; vp.Height = fillH;
            break;
        }
        case FillMode::Center: {
            float cx = screenW * 0.5f;
            float cy = screenH * 0.5f;
            float hw = static_cast<float>(texW) * 0.5f;
            float hh = static_cast<float>(texH) * 0.5f;
            vp.TopLeftX = cx - hw; vp.TopLeftY = cy - hh;
            vp.Width = static_cast<float>(texW);
            vp.Height = static_cast<float>(texH);
            scissor.left   = std::max(0L, static_cast<LONG>(vp.TopLeftX));
            scissor.top    = std::max(0L, static_cast<LONG>(vp.TopLeftY));
            scissor.right  = std::min(static_cast<LONG>(m_width),  static_cast<LONG>(vp.TopLeftX + vp.Width));
            scissor.bottom = std::min(static_cast<LONG>(m_height), static_cast<LONG>(vp.TopLeftY + vp.Height));
            break;
        }
        case FillMode::Tile:
            break;
    }
}

} // namespace rex::wallpaper

``

## src/graphics/renderer/D3D12Renderer.hpp

``cpp
#pragma once

#include "Device.hpp"
#include "SwapChain.hpp"
#include "CommandQueue.hpp"
#include "DescriptorHeapManager.hpp"
#include "wallpaper/decoder/WICTextureLoader.hpp"
#include "wallpaper/decoder/FfmpegVideoDecoder.hpp"

#include <shared/Result.hpp>
#include <rex/wallpaper/IWallpaperEngine.hpp>

#include <array>
#include <mutex>
#include <atomic>
#include <string_view>
#include <DirectXMath.h>

namespace rex::wallpaper {
using namespace rex::graphics;

// ─── D3D12Renderer ─────────────────────────────────────────────────────────
//
// Wallpaper-focused D3D12 renderer. Renders a fullscreen textured quad
// with configurable fill modes. For video playback, supports efficient
// per-frame texture updates via UpdateTexturePixels().
//
// Uses the shared rex::graphics primitives (Device, SwapChain, CommandQueue).

class D3D12Renderer {
public:
    D3D12Renderer() = default;
    ~D3D12Renderer();

    shared::Result<void> Initialize(HWND targetWindow, int width, int height);
    /// Initialize on an existing device (multi-monitor compositors share
    /// the engine's device so shared-handle video textures stay valid).
    shared::Result<void> Initialize(HWND targetWindow, int width, int height,
                                    ID3D12Device* adoptDevice);
    void Shutdown();
    void Resize(int width, int height);

    // ─── Texture Loading ────────────────────────────────────────────────
    shared::Result<void> LoadTexture(std::string_view path);
    shared::Result<void> UpdateTexturePixels(
        const uint8_t* pixels, int width, int height, int rowPitch);
    void ClearTexture();
    bool HasTexture() const { return m_hasTexture; }

    // ─── Video (shared-handle) rendering ────────────────────────────────
    /// Binds the GPU frame described by `frame` (a D3D11 pool slot shared
    /// via NT handle) for the next RenderFrame(). `waitFence` is the shared
    /// D3D11/D3D12 fence the command queue must wait on (value
    /// frame.fenceValue) before sampling. `frame.colorInfo` drives the
    /// color pipeline (SDR/PQ/HLG decode, primaries, HDR metadata).
    /// MUST be called between BeginFrame() and EndFrame() (same convention
    /// as UpdateTexturePixels).
    shared::Result<void> DrawVideoFrame(const VideoFrame& frame,
                                        ID3D12Fence* waitFence);
    /// Drops all opened video slot resources. MUST NOT be called between
    /// BeginFrame() and EndFrame() (it waits for the GPU).
    void ClearVideoFrame();
    bool HasVideoFrame() const { return m_hasVideoFrame; }

    /// HDR status and display luminance (from the swap chain; valid only
    /// when HDR is active — see SwapChain::IsHDRActive / GetDisplayMaxNits).
    bool  IsHDRActive() const { return m_swapChain.IsHDRActive(); }
    float GetDisplayMaxNits() const { return m_swapChain.GetDisplayMaxNits(); }

    // ─── Fill Mode ──────────────────────────────────────────────────────
    void SetFillMode(FillMode mode) { m_fillMode = mode; }
    FillMode GetFillMode() const { return m_fillMode; }

    // ─── Video Adjustments ──────────────────────────────────────────────
    /// Brightness/contrast/saturation applied by the video shader in
    /// linear space. Identity for all three is 1.0 (the engine clamps
    /// user values to 0..2).
    void SetVideoAdjustments(float brightness, float contrast, float saturation) {
        m_brightness = brightness;
        m_contrast = contrast;
        m_saturation = saturation;
    }

    // ─── Force SDR (diagnostic) ─────────────────────────────────────
    void SetForceSdr(bool force) { m_forceSdr = force; }
    bool IsForceSdr() const { return m_forceSdr; }

    // ─── Transform (rotation / flip) ────────────────────────────────────
    /// Rotation in degrees around the screen center + horizontal/vertical
    /// mirroring. Applied to the quad in clip space via a vertex-shader
    /// matrix (root constant b1).
    void SetRotation(float degrees);
    void SetFlip(bool horizontal, bool vertical);

    // ─── Custom pixel shader hook ───────────────────────────────────────
    /// Compiles `hlslSource` (entry "main", ps_5_0) and swaps it into the
    /// pipeline. The shader must bind the same t0 texture / s0 sampler /
    /// b0 color constants as the default PS (the root signature is
    /// unchanged). Reverts to the default on compile/PSO failure.
    bool SetCustomPixelShader(const std::string& hlslSource);
    /// Revert to the built-in SDR/HDR pixel shader.
    void ClearCustomPixelShader();

    // ─── Frame Rendering ────────────────────────────────────────────────
    void BeginFrame();
    void RenderFrame();
    void EndFrame();
    void SetClearColor(float r, float g, float b);

    bool IsInitialized()   const { return m_initialized; }
    bool IsDeviceRemoved() const { return m_deviceRemoved; }

    // ─── Telemetry counters (for the memory-retention diagnostics) ─────
    uint64_t GetTextureBytes()        const { return m_textureBytes; }
    uint64_t GetUploadBytes()         const { return m_uploadBytes; }
    uint64_t GetTextureRecreations()  const { return m_textureRecreations; }
    uint64_t GetUploadCount()         const { return m_uploads; }
    uint64_t GetAllocatorsPerUpload() const { return m_allocatorsPerUpload; }

    // ─── Access to shared graphics primitives ──────────────────────────
    graphics::Device&       GetDevice()       { return m_device; }
    const graphics::Device& GetDevice() const { return m_device; }

private:
    // ─── Initialization helpers ─────────────────────────────────────────
    bool CreateRootSignature();
    bool CreatePipelineState(DXGI_FORMAT rtvFormat,
                             Microsoft::WRL::ComPtr<ID3D12PipelineState>& outPs);
    bool CreatePipelineStateWithPs(DXGI_FORMAT rtvFormat,
                                   const void* psBytecode, SIZE_T psSize,
                                   Microsoft::WRL::ComPtr<ID3D12PipelineState>& outPs);
    bool CreateVertexBuffer();

    /// Rebuilds m_transformMatrix from rotation/flip (row-major, matches
    /// the vertex shader's row-vector mul(v, M)). Caller holds m_renderMutex.
    void UpdateTransformMatrix();

    // ─── Color pipeline ─────────────────────────────────────────────────
    /// Fills the root-constant payload (b0) from the active frame's
    /// colorimetry plus display settings.
    void BuildColorConstants(float cb[16], bool hdrOutput) const;

    // ─── Texture Upload ────────────────────────────────────────────────
    bool UploadTextureToGPU(const TextureData& data);

    // ─── Fill Mode Viewport ────────────────────────────────────────────
    void ComputeFillViewport(D3D12_VIEWPORT& vp, D3D12_RECT& scissor) const;

    // ─── Shared primitives ─────────────────────────────────────────────
    graphics::Device       m_device;
    graphics::SwapChain    m_swapChain;
    graphics::CommandQueue m_commandQueue;

    // ─── Per-frame command state ───────────────────────────────────────
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    m_commandAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commandList;

    // ─── Pipeline state ────────────────────────────────────────────────
    Microsoft::WRL::ComPtr<ID3D12RootSignature>  m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_pipelineState;     // SDR back buffer
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_pipelineStateHdr;  // R16F back buffer
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_customPSO;         // custom PS (SDR)
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_customPSOHdr;      // custom PS (R16F)
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_vertexBuffer;

    // ─── SRV heap ──────────────────────────────────────────────────────
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap;

    // ─── Texture state ─────────────────────────────────────────────────
    Microsoft::WRL::ComPtr<ID3D12Resource>  m_texture;
    Microsoft::WRL::ComPtr<ID3D12Resource>  m_textureUploadBuffer;
    D3D12_GPU_DESCRIPTOR_HANDLE            m_textureSRVGpuHandle{};
    bool m_hasTexture = false;
    int  m_texWidth  = 0;
    int  m_texHeight = 0;

    // ─── Video (shared-handle) state ───────────────────────────────────
    // Opened D3D12 views of the decoder's BGRA pool slots. One SRV per
    // slot, laid out in the SRV heap after the static texture (slots 1..8).
    struct VideoSlotResource {
        HANDLE sharedHandle = nullptr;
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        D3D12_GPU_DESCRIPTOR_HANDLE srvGpu{};
    };
    static constexpr UINT kMaxVideoSlots = 8;
    std::array<VideoSlotResource, kMaxVideoSlots> m_videoSlots;
    UINT   m_videoSlotCount = 0;
    HANDLE m_activeVideoHandle = nullptr;
    bool   m_hasVideoFrame = false;
    int    m_videoWidth  = 0;
    int    m_videoHeight = 0;

    // ─── Color pipeline state ──────────────────────────────────────────
    VideoColorInfo m_activeColorInfo;   // colorimetry of the bound frame
    bool m_isHdrOutput = false;         // swap chain is R16F + PQ/BT.2020
    bool m_forceSdr = false;            // --force-sdr: override hdrOutput to 0
    float m_displayPeakNits = 80.0f;    // tone-mapping target (display or
                                        // SDR reference white in nits)
    float m_inputFormat = 0.0f;         // shader input format (0=NV12, 1=RGBA16, 2=P010)
    float m_debugMode = 1.0f;           // shader debug (0=normal, 1=Y-only, 2=UV-only)

    // ─── Fill mode ─────────────────────────────────────────────────────
    FillMode m_fillMode = FillMode::Fill;

    // ─── Transform state ───────────────────────────────────────────────
    float m_rotation = 0.0f;
    bool  m_flipH = false;
    bool  m_flipV = false;
    DirectX::XMFLOAT4X4 m_transformMatrix = {
        1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

    // ─── Custom pixel shader state ─────────────────────────────────────
    std::string m_customPsSource;
    bool m_customPsActive = false;

    // ─── Video adjustments (identity = 1.0) ────────────────────────────
    float m_brightness = 1.0f;
    float m_contrast   = 1.0f;
    float m_saturation = 1.0f;

    // ─── Dimensions & state ───────────────────────────────────────────
    int   m_width = 0;
    int   m_height = 0;
    float m_clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    bool  m_initialized  = false;
    bool  m_deviceRemoved = false;

    // ─── Thread safety ────────────────────────────────────────────────
    mutable std::mutex m_renderMutex;

    // ─── Telemetry ─────────────────────────────────────────────────────
    std::atomic<uint64_t> m_textureBytes{0};
    std::atomic<uint64_t> m_uploadBytes{0};
    std::atomic<uint64_t> m_textureRecreations{0};
    std::atomic<uint64_t> m_uploads{0};
    // Cumulative command allocators/lists created inside upload paths.
    // Flatlines after warm-up (P5 ring); growth during playback = regression.
    std::atomic<uint64_t> m_allocatorsPerUpload{0};

    // ─── Persistent copy/upload contexts (P5) ─────────────────────────
    // Ring of allocator+list pairs reused by the software-copy and texture
    // upload paths. Created once in Initialize(); steady-state uploads
    // perform ZERO command-buffer allocations. Each path executes then
    // WaitForGPUs, so entries are always GPU-idle before reuse.
    static constexpr UINT kCopyContextCount = 4;
    struct CopyContext {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    };
    CopyContext m_copyContexts[kCopyContextCount]{};
    UINT        m_copyContextNext = 0;
    /// Fetches the next ring slot, creating it lazily (warm-up/device-loss
    /// recovery only). List comes back closed-and-ready for Reset().
    shared::Result<void> AcquireCopyContext(
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator>& allocatorOut,
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>& listOut);

    // ─── Vertex layout ────────────────────────────────────────────────
    struct Vertex { float x, y, u, v; };
};

} // namespace rex::wallpaper

``

## src/graphics/renderer/SwapChain.cpp

``cpp
#include "SwapChain.hpp"
#include <shared/Logger.hpp>
#include <exception>

namespace rex::graphics {

SwapChain::~SwapChain() {
    try {
        Shutdown();
    } catch (const std::exception& ex) {
        REX_LOG_ERROR("SwapChain", "~SwapChain exception: {}", ex.what());
    } catch (...) {
        REX_LOG_ERROR("SwapChain", "~SwapChain unknown exception");
    }
}

shared::Result<void> SwapChain::Initialize(
    ID3D12CommandQueue* commandQueue,
    ID3D12Device* device,
    HWND hwnd,
    int width, int height,
    UINT bufferCount) {

    if (m_initialized) {
        return shared::Result<void>(shared::ErrorCode::AlreadyInitialized,
                                    "SwapChain already initialized");
    }

    m_bufferCount = bufferCount;

    Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        return shared::Result<void>(shared::ErrorCode::DriverError,
                                    "CreateDXGIFactory2 failed");
    }

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.BufferCount  = bufferCount;
    desc.Width        = static_cast<UINT>(std::max(width, 1));
    desc.Height       = static_cast<UINT>(std::max(height, 1));
    desc.Format       = m_format;
    desc.BufferUsage  = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.SwapEffect   = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.SampleDesc.Count = 1;
    desc.Flags        = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain1;
    hr = factory->CreateSwapChainForHwnd(
        commandQueue, hwnd, &desc, nullptr, nullptr, &swapChain1);
    if (FAILED(hr)) {
        return shared::Result<void>(shared::ErrorCode::DriverError,
                                    "CreateSwapChainForHwnd failed");
    }

    hr = swapChain1.As(&m_swapChain);
    if (FAILED(hr)) {
        return shared::Result<void>(shared::ErrorCode::DriverError,
                                    "Failed to get IDXGISwapChain4");
    }

    // HDR10 vs SDR: pick the back-buffer format + color space from the
    // output the window currently sits on.
    UpdateHdrSelection();
    REX_LOG_INFO("SwapChain", "Back buffer: {} ({})",
                 m_isHdr ? "R16G16B16A16_FLOAT HDR10" : "R8G8B8A8_UNORM SDR",
                 m_isHdr ? "PQ/BT.2020" : "sRGB/BT.709");

    // Set max frame latency to 2 for smooth vsync-aligned pacing
    Microsoft::WRL::ComPtr<IDXGISwapChain2> swapChain2;
    if (SUCCEEDED(swapChain1.As(&swapChain2))) {
        swapChain2->SetMaximumFrameLatency(2);
        m_frameLatencyWaitable = swapChain2->GetFrameLatencyWaitableObject();
        REX_LOG_INFO("SwapChain", "Frame latency waitable object created");
    }

    // Disable automatic Alt+Enter handling
    Microsoft::WRL::ComPtr<IDXGIFactory> parentFactory;
    if (SUCCEEDED(m_swapChain->GetParent(IID_PPV_ARGS(&parentFactory)))) {
        parentFactory->MakeWindowAssociation(
            hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    }

    // Create RTV heap and render target views
    auto rtvResult = CreateRenderTargetViews(device);
    if (rtvResult.IsError()) {
        return rtvResult;
    }

    m_initialized = true;
    REX_LOG_INFO("SwapChain", "Created: {}x{}x{}", width, height, bufferCount);
    return shared::Result<void>();
}

void SwapChain::Shutdown() {
    if (!m_initialized) return;

    for (auto& rt : m_renderTargets) rt.Reset();
    m_rtvHandles[0] = m_rtvHandles[1] = m_rtvHandles[2] = {};
    m_rtvHeap.Reset();
    m_swapChain.Reset();

    if (m_frameLatencyWaitable) {
        CloseHandle(m_frameLatencyWaitable);
        m_frameLatencyWaitable = nullptr;
    }

    m_initialized = false;
}

shared::Result<void> SwapChain::Resize(
    ID3D12Device* device, int width, int height) {

    if (!m_swapChain) {
        return shared::Result<void>(shared::ErrorCode::NotInitialized,
                                    "SwapChain not initialized");
    }

    // Release existing RTVs
    for (auto& rt : m_renderTargets) rt.Reset();

    // Re-evaluate HDR selection (the window may have moved to a different
    // output); may change the buffer format.
    UpdateHdrSelection();

    HRESULT hr = m_swapChain->ResizeBuffers(
        m_bufferCount,
        static_cast<UINT>(std::max(width, 1)),
        static_cast<UINT>(std::max(height, 1)),
        m_format, 0);

    if (FAILED(hr)) {
        return shared::Result<void>(shared::ErrorCode::DriverError,
                                    "ResizeBuffers failed");
    }

    m_swapChain->SetColorSpace1(m_isHdr ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
                                        : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);

    auto rtvResult = CreateRenderTargetViews(device);
    if (rtvResult.IsError()) {
        return rtvResult;
    }

    REX_LOG_INFO("SwapChain", "Resized to {}x{}", width, height);
    return shared::Result<void>();
}

HRESULT SwapChain::Present(UINT syncInterval, UINT flags) {
    return m_swapChain->Present(syncInterval, flags);
}

void SwapChain::WaitForFrame() const {
    if (m_frameLatencyWaitable) {
        WaitForSingleObjectEx(m_frameLatencyWaitable, 1000, FALSE);
    }
}

// ─── Private ───────────────────────────────────────────────────────────────

void SwapChain::UpdateHdrSelection() {
    if (!m_swapChain) return;
    if (m_forceSdr) return;  // --force-sdr: stay on SDR format

    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    if (FAILED(m_swapChain->GetContainingOutput(&output))) return;
    Microsoft::WRL::ComPtr<IDXGIOutput6> out6;
    DXGI_OUTPUT_DESC1 desc1 = {};
    if (FAILED(output.As(&out6)) || FAILED(out6->GetDesc1(&desc1))) return;

    // HDR10-capable outputs advertise the PQ/BT.2020 color space. scRGB
    // (advanced color) is intentionally treated as SDR for now.
    const bool hdr = desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    const DXGI_FORMAT fmt = hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                : DXGI_FORMAT_R8G8B8A8_UNORM;
    if (hdr == m_isHdr && fmt == m_format) return;

    m_isHdr = hdr;
    m_format = fmt;
    m_hdrActive = hdr;
    m_displayMaxNits = hdr ? static_cast<float>(desc1.MaxLuminance) : 0.0f;
    m_displayMinNits = hdr ? static_cast<float>(desc1.MinLuminance) : 0.0f;

    // A format change requires ResizeBuffers (0,0 = keep current size);
    // the color space is a per-swap-chain property.
    HRESULT hr = m_swapChain->ResizeBuffers(m_bufferCount, 0, 0, fmt,
        DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
    if (FAILED(hr)) {
        REX_LOG_WARN("SwapChain", "format change resize failed: {:08x}",
                     static_cast<unsigned long>(hr));
        m_isHdr = false;
        m_format = DXGI_FORMAT_R8G8B8A8_UNORM;
        m_hdrActive = false;
        m_displayMaxNits = 0.0f;
        m_displayMinNits = 0.0f;
        return;
    }
    m_swapChain->SetColorSpace1(hdr ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
                                    : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    REX_LOG_INFO("SwapChain", "HDR10 output {}", hdr ? "enabled" : "disabled");
}

shared::Result<void> SwapChain::CreateRenderTargetViews(ID3D12Device* device) {
    UINT rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // (Re)create RTV heap
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
    rtvDesc.NumDescriptors = m_bufferCount;
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;

    m_rtvHeap.Reset();
    HRESULT hr = device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m_rtvHeap));
    if (FAILED(hr)) {
        return shared::Result<void>(shared::ErrorCode::OutOfMemory,
                                    "CreateDescriptorHeap (RTV) failed");
    }

    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();

    for (UINT i = 0; i < m_bufferCount; ++i) {
        m_renderTargets[i].Reset();
        hr = m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_renderTargets[i]));
        if (FAILED(hr)) {
            return shared::Result<void>(shared::ErrorCode::DriverError,
                                        "GetBuffer failed for slot " + std::to_string(i));
        }
        m_rtvHandles[i] = handle;
        device->CreateRenderTargetView(m_renderTargets[i].Get(), nullptr, handle);
        handle.ptr += rtvDescriptorSize;
    }

    return shared::Result<void>();
}

} // namespace rex::graphics

``

## src/graphics/renderer/SwapChain.hpp

``cpp
#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <shared/Result.hpp>

#include <cstdint>

namespace rex::graphics {

// ─── SwapChain ──────────────────────────────────────────────────────────────
//
// Manages IDXGISwapChain4 creation, RTV descriptor heap, render target views,
// resize, present, and frame latency waitable object for vsync-aligned pacing.
//
// The swap chain is created against a command queue (ID3D12CommandQueue*)
// passed at construction time.

class SwapChain {
public:
    SwapChain() = default;
    ~SwapChain();

    /// Create the swap chain for the given HWND.
    /// @param commandQueue The command queue used for presenting.
    /// @param hwnd         The target window.
    /// @param width,height Initial dimensions.
    /// @param bufferCount  Number of back buffers (default 3).
    shared::Result<void> Initialize(
        ID3D12CommandQueue* commandQueue,
        ID3D12Device* device,
        HWND hwnd,
        int width,
        int height,
        UINT bufferCount = 3);

    void Shutdown();

    /// Resize buffers (called on WM_SIZE).
    shared::Result<void> Resize(
        ID3D12Device* device, int width, int height);

    /// Present the current frame. Returns the HRESULT from Present.
    /// Caller should check IsDeviceRemoved() if the result is an error.
    HRESULT Present(UINT syncInterval = 1, UINT flags = 0);

    /// Wait for the frame latency waitable object (vsync-aligned pacing).
    /// Returns immediately if the waitable object was not created.
    void WaitForFrame() const;

    /// Get current back buffer index (queried live from the swap chain).
    UINT GetCurrentIndex() const {
        return m_swapChain ? m_swapChain->GetCurrentBackBufferIndex() : 0;
    }

    /// Get the current back buffer resource.
    ID3D12Resource* GetBuffer(UINT index) const {
        return (index < m_bufferCount) ? m_renderTargets[index].Get() : nullptr;
    }

    /// Get the RTV handle for a back buffer.
    D3D12_CPU_DESCRIPTOR_HANDLE GetRTVHandle(UINT index) const {
        return m_rtvHandles[index];
    }

    /// Get the RTV descriptor heap.
    ID3D12DescriptorHeap* GetRTVHeap() const { return m_rtvHeap.Get(); }

    UINT GetBufferCount() const { return m_bufferCount; }
    bool IsValid() const { return m_swapChain != nullptr; }

    /// True when the swap chain targets an HDR10 output and presents in
    /// R16G16B16A16_FLOAT + PQ (ST.2084) / BT.2020.
    bool IsHDR() const { return m_isHdr; }
    DXGI_FORMAT GetBackBufferFormat() const { return m_format; }

    /// Display luminance capability (nits) as reported by DXGI_OUTPUT_DESC1.
    /// Valid only when IsHDRActive() (falls back to 0 otherwise).
    float GetDisplayMaxNits() const { return m_displayMaxNits; }
    float GetDisplayMinNits() const { return m_displayMinNits; }
    /// True when HDR10 presentation is actually active on this swap chain.
    bool IsHDRActive() const { return m_hdrActive; }

    /// Force SDR mode: prevents UpdateHdrSelection from switching to R16F/PQ.
    void SetForceSdr(bool force) { m_forceSdr = force; }
    bool IsForceSdr() const { return m_forceSdr; }

private:
    shared::Result<void> CreateRenderTargetViews(ID3D12Device* device);
    /// Queries the output the swap chain is currently on and picks the
    /// back-buffer format + color space (HDR10 vs SDR). No-op if the
    /// selection matches the current one.
    void UpdateHdrSelection();

    Microsoft::WRL::ComPtr<IDXGISwapChain4>     m_swapChain;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_renderTargets[3];
    D3D12_CPU_DESCRIPTOR_HANDLE                 m_rtvHandles[3];

    HANDLE  m_frameLatencyWaitable = nullptr;
    UINT    m_bufferCount = 3;
    DXGI_FORMAT m_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    bool    m_isHdr = false;
    float   m_displayMaxNits = 0.0f;
    float   m_displayMinNits = 0.0f;
    bool    m_hdrActive = false;
    bool    m_forceSdr = false;
    bool    m_initialized = false;
};

} // namespace rex::graphics

``

## src/graphics/renderer/Device.cpp

``cpp
#include "Device.hpp"
#include <shared/Logger.hpp>

namespace rex::graphics {

Device::~Device() = default;

shared::Result<void> Device::Initialize(D3D_FEATURE_LEVEL minimumFeatureLevel) {
    if (m_device) {
        return shared::Result<void>(shared::ErrorCode::AlreadyInitialized,
                                    "Device already initialized");
    }

#ifdef _DEBUG
    {
        Microsoft::WRL::ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
            debugController->EnableDebugLayer();
            REX_LOG_INFO("Device", "D3D12 debug layer enabled");
        }
    }
#endif

    // ── Create DXGI factory and enumerate adapters ──────────────────────
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        return shared::Result<void>(shared::ErrorCode::DriverError,
                                    "CreateDXGIFactory2 failed");
    }

    // Prefer hardware adapter
    Microsoft::WRL::ComPtr<IDXGIAdapter4> adapter;
    for (UINT i = 0; factory->EnumAdapterByGpuPreference(
             i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
             IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC3 desc;
        if (SUCCEEDED(adapter->GetDesc3(&desc))) {
            // Skip software adapters
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

            // Check if this adapter supports D3D12 at the requested level
            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), minimumFeatureLevel,
                                            __uuidof(ID3D12Device), nullptr))) {
                m_adapter = adapter;
                REX_LOG_INFO("Device", "Selected adapter: {} (D3D_FEATURE_LEVEL_{}.{})",
                             reinterpret_cast<const char*>(desc.Description),
                             (minimumFeatureLevel >> 12) & 0xF,
                             (minimumFeatureLevel >> 8) & 0xF);
                break;
            }
        }
        adapter.Reset();
    }

    // Fall back to default adapter if no hardware adapter matched
    if (!m_adapter) {
        hr = D3D12CreateDevice(nullptr, minimumFeatureLevel, IID_PPV_ARGS(&m_device));
    } else {
        hr = D3D12CreateDevice(m_adapter.Get(), minimumFeatureLevel, IID_PPV_ARGS(&m_device));
    }

    if (FAILED(hr)) {
        return shared::Result<void>(shared::ErrorCode::HardwareNotSupported,
                                    "D3D12CreateDevice failed");
    }

    // ── Query feature level ─────────────────────────────────────────────
    D3D12_FEATURE_DATA_FEATURE_LEVELS featLevels = {};
    D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0
    };
    featLevels.NumFeatureLevels = ARRAYSIZE(levels);
    featLevels.pFeatureLevelsRequested = levels;

    if (SUCCEEDED(m_device->CheckFeatureSupport(
            D3D12_FEATURE_FEATURE_LEVELS, &featLevels, sizeof(featLevels)))) {
        m_maxFeatureLevel = featLevels.MaxSupportedFeatureLevel;
    }

    REX_LOG_INFO("Device", "D3D12 device created (max feature level: {}.{})",
                 (m_maxFeatureLevel >> 12) & 0xF,
                 (m_maxFeatureLevel >> 8) & 0xF);

    m_device->SetName(L"REX D3D12 Device");
    return shared::Result<void>();
}

shared::Result<void> Device::InitializeAdopted(ID3D12Device* device) {
    if (m_device) {
        return shared::Result<void>(shared::ErrorCode::AlreadyInitialized,
                                    "Device already initialized");
    }
    if (!device) {
        return shared::Result<void>(shared::ErrorCode::InvalidArgument,
                                    "Adopted device is null");
    }

    m_device = device;

    // Query the adapter and max feature level for telemetry parity with
    // self-created devices.
    Microsoft::WRL::ComPtr<IDXGIDevice4> dxgiDevice;
    if (SUCCEEDED(m_device.As(&dxgiDevice))) {
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
            adapter.As(&m_adapter);
        }
    }

    D3D12_FEATURE_DATA_FEATURE_LEVELS featLevels = {};
    // SDK 26100+: feature levels use the shared D3D_FEATURE_LEVEL enum
    // (D3D12_FEATURE_LEVEL no longer exists).
    D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0
    };
    featLevels.NumFeatureLevels = ARRAYSIZE(levels);
    featLevels.pFeatureLevelsRequested = levels;
    if (SUCCEEDED(m_device->CheckFeatureSupport(
            D3D12_FEATURE_FEATURE_LEVELS, &featLevels, sizeof(featLevels)))) {
        m_maxFeatureLevel = featLevels.MaxSupportedFeatureLevel;
    }

    REX_LOG_INFO("Device", "Adopted D3D12 device (max feature level: {}.{})",
                 (m_maxFeatureLevel >> 12) & 0xF,
                 (m_maxFeatureLevel >> 8) & 0xF);
    return shared::Result<void>();
}

// ─── Factory helpers ───────────────────────────────────────────────────────

Microsoft::WRL::ComPtr<ID3D12CommandAllocator> Device::CreateCommandAllocator(
    D3D12_COMMAND_LIST_TYPE type) const {

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    HRESULT hr = m_device->CreateCommandAllocator(type, IID_PPV_ARGS(&allocator));
    if (FAILED(hr)) {
        REX_LOG_ERROR("Device", "CreateCommandAllocator failed: {:08x}", hr);
    }
    return allocator;
}

Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> Device::CreateCommandList(
    ID3D12CommandAllocator* allocator, ID3D12PipelineState* initialPso) const {

    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    HRESULT hr = m_device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, initialPso,
        IID_PPV_ARGS(&list));
    if (FAILED(hr)) {
        REX_LOG_ERROR("Device", "CreateCommandList failed: {:08x}", hr);
    }
    return list;
}

Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> Device::CreateDescriptorHeap(
    D3D12_DESCRIPTOR_HEAP_TYPE type, UINT numDescriptors,
    D3D12_DESCRIPTOR_HEAP_FLAGS flags) const {

    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.NumDescriptors = numDescriptors;
    desc.Type = type;
    desc.Flags = flags;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    HRESULT hr = m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap));
    if (FAILED(hr)) {
        REX_LOG_ERROR("Device", "CreateDescriptorHeap failed: {:08x}", hr);
    }
    return heap;
}

Microsoft::WRL::ComPtr<ID3D12RootSignature> Device::CreateRootSignature(
    const void* serializedBlob, SIZE_T blobSize) const {

    Microsoft::WRL::ComPtr<ID3D12RootSignature> signature;
    HRESULT hr = m_device->CreateRootSignature(
        0, serializedBlob, blobSize, IID_PPV_ARGS(&signature));
    if (FAILED(hr)) {
        REX_LOG_ERROR("Device", "CreateRootSignature failed: {:08x}", hr);
    }
    return signature;
}

Microsoft::WRL::ComPtr<ID3D12PipelineState> Device::CreateGraphicsPipelineState(
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc) const {

    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
    HRESULT hr = m_device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
    if (FAILED(hr)) {
        REX_LOG_ERROR("Device", "CreateGraphicsPipelineState failed: {:08x}", hr);
    }
    return pso;
}

Microsoft::WRL::ComPtr<ID3D12Resource> Device::CreateCommittedResource(
    const D3D12_HEAP_PROPERTIES& heapProps, D3D12_HEAP_FLAGS flags,
    const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES initial,
    const D3D12_CLEAR_VALUE* clearValue) const {

    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    HRESULT hr = m_device->CreateCommittedResource(
        &heapProps, flags, &desc, initial, clearValue, IID_PPV_ARGS(&resource));
    if (FAILED(hr)) {
        REX_LOG_ERROR("Device", "CreateCommittedResource failed: {:08x}", hr);
    }
    return resource;
}

Microsoft::WRL::ComPtr<IDXGIFactory6> Device::CreateFactory() const {
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        REX_LOG_ERROR("Device", "CreateDXGIFactory2 failed: {:08x}", hr);
    }
    return factory;
}

} // namespace rex::graphics

``

## src/graphics/renderer/Device.hpp

``cpp
#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <shared/Result.hpp>

#include <cstdint>
#include <string>

namespace rex::graphics {

// ─── Device ─────────────────────────────────────────────────────────────────
//
// Wraps ID3D12Device creation and provides factory methods for common D3D12
// objects (command allocators, command lists, descriptor heaps, root
// signatures, pipeline states, committed resources).
//
// Thread-safe: the underlying device is free-threaded.

class Device {
public:
    Device() = default;
    ~Device();

    /// Create the D3D12 device with debug layer in Debug builds.
    /// @param minimumFeatureLevel Minimum feature level (default 11_0).
    shared::Result<void> Initialize(
        D3D_FEATURE_LEVEL minimumFeatureLevel = D3D_FEATURE_LEVEL_11_0);

    /// Adopt an existing D3D12 device (shared with other components).
    /// Guarantees every renderer uses the SAME device — required for
    /// cross-device shared handles (video pool textures/fence) to stay
    /// valid, regardless of adapter topology.
    shared::Result<void> InitializeAdopted(ID3D12Device* device);

    /// Returns the maximum supported feature level.
    D3D_FEATURE_LEVEL MaxFeatureLevel() const { return m_maxFeatureLevel; }

    /// Returns the DXGI adapter used by this device.
    Microsoft::WRL::ComPtr<IDXGIAdapter4> Adapter() const { return m_adapter; }

    /// Get the underlying D3D12 device.
    ID3D12Device* Get() const { return m_device.Get(); }

    // ─── Factory helpers ──────────────────────────────────────────────────
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT) const;

    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> CreateCommandList(
        ID3D12CommandAllocator* allocator,
        ID3D12PipelineState* initialPso = nullptr) const;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> CreateDescriptorHeap(
        D3D12_DESCRIPTOR_HEAP_TYPE type,
        UINT numDescriptors,
        D3D12_DESCRIPTOR_HEAP_FLAGS flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE) const;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> CreateRootSignature(
        const void* serializedBlob, SIZE_T blobSize) const;

    Microsoft::WRL::ComPtr<ID3D12PipelineState> CreateGraphicsPipelineState(
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc) const;

    Microsoft::WRL::ComPtr<ID3D12Resource> CreateCommittedResource(
        const D3D12_HEAP_PROPERTIES& heapProps,
        D3D12_HEAP_FLAGS flags,
        const D3D12_RESOURCE_DESC& desc,
        D3D12_RESOURCE_STATES initial,
        const D3D12_CLEAR_VALUE* clearValue = nullptr) const;

    Microsoft::WRL::ComPtr<IDXGIFactory6> CreateFactory() const;

    UINT GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE type) const {
        return m_device->GetDescriptorHandleIncrementSize(type);
    }

    // ─── Device removal ──────────────────────────────────────────────────
    HRESULT GetDeviceRemovedReason() const {
        return m_device ? m_device->GetDeviceRemovedReason() : S_OK;
    }

    bool IsValid() const { return m_device != nullptr; }

private:
    Microsoft::WRL::ComPtr<ID3D12Device>    m_device;
    Microsoft::WRL::ComPtr<IDXGIAdapter4>   m_adapter;
    D3D_FEATURE_LEVEL                       m_maxFeatureLevel = D3D_FEATURE_LEVEL_11_0;
};

} // namespace rex::graphics

``

## src/graphics/renderer/CommandQueue.cpp

``cpp
#include "CommandQueue.hpp"
#include <shared/Logger.hpp>
#include <exception>

namespace rex::graphics {

CommandQueue::~CommandQueue() {
    try {
        Shutdown();
    } catch (const std::exception& ex) {
        REX_LOG_ERROR("CommandQueue", "~CommandQueue exception: {}", ex.what());
    } catch (...) {
        REX_LOG_ERROR("CommandQueue", "~CommandQueue unknown exception");
    }
}

shared::Result<void> CommandQueue::Initialize(ID3D12Device* device) {
    if (m_queue) {
        return shared::Result<void>(shared::ErrorCode::AlreadyInitialized,
                                    "CommandQueue already initialized");
    }
    if (!device) {
        return shared::Result<void>(shared::ErrorCode::InvalidArgument,
                                    "Device is null");
    }

    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT;
    desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    desc.Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE;

    HRESULT hr = device->CreateCommandQueue(&desc, IID_PPV_ARGS(&m_queue));
    if (FAILED(hr)) {
        return shared::Result<void>(shared::ErrorCode::DriverError,
                                    "CreateCommandQueue failed");
    }

    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
    if (FAILED(hr)) {
        return shared::Result<void>(shared::ErrorCode::DriverError,
                                    "CreateFence failed");
    }

    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent) {
        return shared::Result<void>(shared::ErrorCode::UnknownError,
                                    "CreateEvent failed");
    }

    REX_LOG_INFO("CommandQueue", "Initialized");
    return shared::Result<void>();
}

void CommandQueue::Shutdown() {
    if (m_queue) {
        WaitForGPU();
    }

    m_queue.Reset();
    m_fence.Reset();

    if (m_fenceEvent) {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }

    m_fenceValue = 0;
}

void CommandQueue::Execute(ID3D12CommandList* const* lists, UINT count) {
    m_queue->ExecuteCommandLists(count, lists);
}

void CommandQueue::Execute(ID3D12CommandList* list) {
    ID3D12CommandList* lists[] = { list };
    m_queue->ExecuteCommandLists(1, lists);
}

void CommandQueue::WaitForGPU() {
    m_fenceValue++;
    HRESULT hr = m_queue->Signal(m_fence.Get(), m_fenceValue);
    if (FAILED(hr)) {
        REX_LOG_ERROR("CommandQueue", "Signal failed: {:08x}", hr);
        return;
    }

    if (m_fence->GetCompletedValue() < m_fenceValue) {
        hr = m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
        if (SUCCEEDED(hr)) {
            WaitForSingleObject(m_fenceEvent, INFINITE);
        }
    }
}

void CommandQueue::WaitForFence(ID3D12Fence* fence, UINT64 value) {
    if (!m_queue || !fence) return;
    m_queue->Wait(fence, value);
}

UINT64 CommandQueue::Signal() {
    m_fenceValue++;
    HRESULT hr = m_queue->Signal(m_fence.Get(), m_fenceValue);
    if (FAILED(hr)) {
        REX_LOG_ERROR("CommandQueue", "Signal failed: {:08x}", hr);
    }
    return m_fenceValue;
}

} // namespace rex::graphics

``

## src/graphics/renderer/CommandQueue.hpp

``cpp
#pragma once

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <shared/Result.hpp>

#include <cstdint>

namespace rex::graphics {

// ─── CommandQueue ───────────────────────────────────────────────────────────
//
// Wraps an ID3D12CommandQueue (DIRECT type) with a fence for GPU
// synchronization. Provides Execute(), WaitForGPU(), and Signal().
//
// Thread-safe: ExecuteCommandLists and Signal are free-threaded on
// the D3D12 command queue.

class CommandQueue {
public:
    CommandQueue() = default;
    ~CommandQueue();

    /// Create a DIRECT command queue on the given device.
    shared::Result<void> Initialize(ID3D12Device* device);

    void Shutdown();

    /// Execute one or more command lists.
    void Execute(ID3D12CommandList* const* lists, UINT count);

    /// Convenience: execute a single command list.
    void Execute(ID3D12CommandList* list);

    /// Block until the GPU has completed all submitted work.
    void WaitForGPU();

    /// Insert a GPU-side wait: the queue blocks until the given fence
    /// reaches `value` before executing subsequent work (orders D3D11
    /// video-engine writes before D3D12 sampling).
    void WaitForFence(ID3D12Fence* fence, UINT64 value);

    /// Signal the fence, returning the new fence value.
    UINT64 Signal();

    /// Get the underlying command queue.
    ID3D12CommandQueue* Get() const { return m_queue.Get(); }

    bool IsValid() const { return m_queue != nullptr; }

private:
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
    Microsoft::WRL::ComPtr<ID3D12Fence>        m_fence;
    HANDLE                                    m_fenceEvent = nullptr;
    UINT64                                    m_fenceValue = 0;
};

} // namespace rex::graphics

``

## src/graphics/renderer/DescriptorHeapManager.cpp

``cpp
#include "DescriptorHeapManager.hpp"
#include <shared/Logger.hpp>

namespace rex::wallpaper {

bool DescriptorHeapManager::Initialize(ID3D12Device* device, UINT capacity) {
    if (!device || capacity == 0) return false;

    m_capacity = capacity;

    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.NumDescriptors = capacity;
    desc.Type            = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.Flags           = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_heap));
    if (FAILED(hr)) {
        REX_LOG_ERROR("HeapMgr", "CreateDescriptorHeap failed: {:08x}", hr);
        return false;
    }

    m_descriptorSize = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    m_cpuStart = m_heap->GetCPUDescriptorHandleForHeapStart();
    m_gpuStart = m_heap->GetGPUDescriptorHandleForHeapStart();

    // Pre-fill free list with all indices (in reverse order so we pop from back)
    m_freeList.reserve(capacity);
    for (UINT i = 0; i < capacity; ++i) {
        m_freeList.push_back(capacity - 1 - i); // stack: 255, 254, ..., 0
    }

    REX_LOG_INFO("HeapMgr", "Initialized descriptor heap with {} slots ({} bytes each)",
                 capacity, m_descriptorSize);
    return true;
}

std::optional<DescriptorAllocation> DescriptorHeapManager::Allocate() {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_freeList.empty()) {
        REX_LOG_ERROR("HeapMgr", "Descriptor heap exhausted ({} slots)", m_capacity);
        return std::nullopt;
    }

    UINT index = m_freeList.back();
    m_freeList.pop_back();

    DescriptorAllocation alloc;
    alloc.index     = index;
    alloc.cpuHandle = { m_cpuStart.ptr + static_cast<SIZE_T>(index) * m_descriptorSize };
    alloc.gpuHandle = { m_gpuStart.ptr + static_cast<SIZE_T>(index) * m_descriptorSize };

    return alloc;
}

void DescriptorHeapManager::Free(DescriptorAllocation alloc) {
    if (alloc.index >= m_capacity) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    m_freeList.push_back(alloc.index);
}

UINT DescriptorHeapManager::GetAllocatedCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_capacity - static_cast<UINT>(m_freeList.size());
}

} // namespace rex::wallpaper

``

## src/graphics/renderer/DescriptorHeapManager.hpp

``cpp
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>
#include <mutex>
#include <optional>

namespace rex::wallpaper {

// ─── DescriptorAllocation ──────────────────────────────────────────────────

struct DescriptorAllocation {
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle{};
    UINT index = UINT_MAX;   // 0-based index within the heap
};

// ─── DescriptorHeapManager ─────────────────────────────────────────────────
//
// Manages a D3D12 shader-visible descriptor heap (CBV_SRV_UAV type).
// Allocates descriptors via a free-list for efficient reuse.
//
// Usage:
//   1. Create with desired capacity (e.g., 256 descriptors)
//   2. Allocate() returns a DescriptorAllocation
//   3. Free(allocation) returns the slot to the free list
//
// Thread-safe for allocation/free from any thread.

class DescriptorHeapManager {
public:
    DescriptorHeapManager() = default;
    ~DescriptorHeapManager() = default;

    /// Initialize the heap. Must be called once with a valid D3D12 device.
    bool Initialize(ID3D12Device* device, UINT capacity = 256);

    /// Allocate a single descriptor. Returns nullopt when full.
    std::optional<DescriptorAllocation> Allocate();

    /// Return a descriptor slot to the free list.
    void Free(DescriptorAllocation alloc);

    /// Direct access to the heap for binding.
    ID3D12DescriptorHeap* GetHeap() const { return m_heap.Get(); }

    UINT GetCapacity() const { return m_capacity; }
    UINT GetAllocatedCount() const;
    UINT GetDescriptorSize() const { return m_descriptorSize; }

private:
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_heap;
    UINT m_capacity = 0;
    UINT m_descriptorSize = 0;

    D3D12_CPU_DESCRIPTOR_HANDLE m_cpuStart{};
    D3D12_GPU_DESCRIPTOR_HANDLE m_gpuStart{};

    std::vector<UINT> m_freeList;       // stack of free indices
    mutable std::mutex m_mutex;
};

} // namespace rex::wallpaper

``

## src/wallpaper/engine/Engine.cpp

``cpp
#include "Engine.hpp"
#include "platform/windows/DesktopHostFactory.hpp"
#include "platform/windows/MonitorEdid.hpp"
#include <iostream>
#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <psapi.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

namespace rex::wallpaper {

namespace {

/// LUID of the adapter that owns the output driving `monitorDeviceName`
/// ("\\.\DISPLAYN", matching MONITORINFOEXW.szDevice / DXGI_OUTPUT_DESC.
/// DeviceName). 0 when not found. Cheap enough for 5s telemetry sampling
/// (a handful of adapter/output GetDesc calls).
uint64_t QueryDisplayAdapterLuid(const std::string& monitorDeviceName) {
    if (monitorDeviceName.empty()) return 0;
    const int needed = MultiByteToWideChar(CP_UTF8, 0, monitorDeviceName.c_str(),
                                           -1, nullptr, 0);
    if (needed <= 0) return 0;
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, monitorDeviceName.c_str(), -1,
                        wide.data(), needed);
    wide.resize(static_cast<size_t>(needed) - 1);

    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 0;

    for (UINT a = 0;; ++a) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(a, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        for (UINT o = 0;; ++o) {
            Microsoft::WRL::ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(o, &output) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC desc{};
            if (FAILED(output->GetDesc(&desc))) continue;
            if (wide != desc.DeviceName) continue;
            DXGI_ADAPTER_DESC1 adesc{};
            if (FAILED(adapter->GetDesc1(&adesc))) return 0;
            return static_cast<uint64_t>(adesc.AdapterLuid.LowPart) |
                   (static_cast<uint64_t>(adesc.AdapterLuid.HighPart) << 32);
        }
    }
    return 0;
}

} // namespace

Engine::Engine() {
    REX_LOG_INFO("Engine", "Engine instance created");
}

Engine::~Engine() {
    if (m_state != EngineState::Uninitialized) {
        Shutdown();
    }
}

shared::Result<void> Engine::Initialize(const WallpaperConfig& config) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_state != EngineState::Uninitialized) {
        return shared::Result<void>(shared::ErrorCode::AlreadyInitialized, "Engine already initialized");
    }

    REX_LOG_INFO("Engine", "Initializing...");

    m_config = config;
    m_sessionId = shared::UUID::Generate();

    if (m_config.enableHardwareAcceleration) {
        REX_LOG_INFO("Engine", "Hardware acceleration enabled");
    } else {
        REX_LOG_INFO("Engine", "Hardware acceleration disabled");
    }

    // Acquire desktop host. "MultiMonitor" is the explicit opt-in for
    // per-monitor wallpapers; anything else auto-picks the single-monitor
    // strategy (WorkerW → Progman → error).
    std::unique_ptr<rex::platform::windows::IDesktopHost> host;
    if (config.desktopHost == "MultiMonitor") {
        host = rex::platform::windows::DesktopHostFactory::Create("MultiMonitor");
        if (!host) {
            REX_LOG_WARN("Engine",
                "MultiMonitor host unavailable, falling back to auto selection");
            host = rex::platform::windows::DesktopHostFactory::CreateBestAvailable();
        }
    } else {
        host = rex::platform::windows::DesktopHostFactory::CreateBestAvailable();
    }
    if (!host) {
        REX_LOG_ERROR("Engine", "Failed to create desktop host");
        return shared::Result<void>(shared::ErrorCode::UnknownError,
            "No desktop hosting strategy available. "
            "Check the log for details on why WorkerW and "
            "Progman both failed.");
    }

    HWND renderWindow = host->GetRenderWindow();
    int width  = host->GetWidth();
    int height = host->GetHeight();

    // Create Renderer
    m_renderer = std::make_unique<D3D12Renderer>();
    auto result = m_renderer->Initialize(renderWindow, width, height);
    if (result.IsError()) {
        REX_LOG_ERROR("Engine", "Failed to initialize renderer: {}", result.Error().message);
        host->Release();
        return result;
    }
    m_renderer->SetForceSdr(m_config.forceSdr);

    // Video backend (FFmpeg D3D11VA -> D3D12 shared textures). Non-fatal:
    // video scenes simply won't play if this fails.
    m_videoManager = std::make_unique<VideoManager>();
    auto vmResult = m_videoManager->Initialize(m_config, m_renderer->GetDevice().Get());
    if (vmResult.IsError()) {
        REX_LOG_WARN("Engine", "VideoManager init failed: {} (video scenes will not play)",
                     vmResult.Error().message);
        m_videoManager.reset();
    }

    // Success — transfer ownership to member
    m_desktopHost = std::move(host);

    // Multi-monitor mode: one compositor per monitor inside the wallpaper
    // layer + restore persisted per-monitor assignments.
    if (m_videoManager) {
        m_videoManager->SetWallpaperHostWindow(
            m_desktopHost->GetWallpaperLayerWindow());
    }
    SetupMonitorCompositors();

    // Start shell monitor for desktop host recovery
    // The callback runs on a background thread — it only sets a flag.
    // The actual recovery happens on the render thread.
    m_desktopHost->StartMonitor([this]() {
        m_recoveryNeeded.store(true, std::memory_order_release);
    });

    // Cache monitor state for hot-plug detection
    auto monitors = EnumerateMonitors();
    m_lastMonitorCount = static_cast<int>(monitors.size());
    for (const auto& m : monitors) {
        if (m.isPrimary) {
            m_lastPrimaryWidth  = m.width;
            m_lastPrimaryHeight = m.height;
            break;
        }
    }

    // Start render thread
    m_running = true;
    m_renderThread = std::thread(&Engine::RenderLoop, this);

    SetState(EngineState::Initialized);
    m_stopwatch.Start();

    REX_LOG_INFO("Engine", "Initialized successfully (session: {})", m_sessionId.ToString().c_str());
    return shared::Result<void>();
}

shared::Result<void> Engine::Shutdown() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_state == EngineState::Uninitialized) {
            // Defensive: clean up anything that may have been allocated during
            // a failed Initialize (e.g. renderer failure after host acquisition).
            if (m_desktopHost) {
                m_desktopHost->Release();
                m_desktopHost.reset();
            }
            m_renderer.reset();
            return shared::Result<void>();
        }

        REX_LOG_INFO("Engine", "Shutting down...");

        SetState(EngineState::ShuttingDown);
        m_desktopHost->StopMonitor();
        m_running = false;
        // NOTE: m_mutex is intentionally released here (end of scope) before
        // joining the render thread. The render thread calls GetPosition() /
        // CheckFullScreenPause() internally which both re-enter m_mutex —
        // joining while holding the lock is a guaranteed deadlock.
    } // <── m_mutex released here

    // The render thread may still be executing its current frame; wait for
    // it to observe m_running==false and exit cleanly.
    if (m_renderThread.joinable()) {
        m_renderThread.join();
    }

    // Render thread is fully stopped — no further concurrent access to
    // m_videoManager / m_renderer / m_desktopHost; no lock needed.
    REX_LOG_INFO("Engine", "Shutdown: video teardown begin");
    if (m_videoManager) {
        m_videoManager->ShutdownMonitorCompositors();
        m_videoManager->Shutdown();
        m_videoManager.reset();
    }
    REX_LOG_INFO("Engine", "Shutdown: video teardown done");
    m_renderer.reset();
    REX_LOG_INFO("Engine", "Shutdown: renderer destroyed");
    m_adapter.reset();
    m_desktopHost->Release();
    m_desktopHost.reset();

    m_playing = false;
    m_currentScene = SceneInfo{};
    m_stopwatch.Stop();

    SetState(EngineState::Uninitialized);
    REX_LOG_INFO("Engine", "Shutdown complete");
    return shared::Result<void>();
}

// ─── Recovery ───────────────────────────────────────────────────────────────

shared::Result<void> Engine::ReinitializeRenderer() {
    REX_LOG_INFO("Engine", "Reinitializing renderer...");

    // Destroy old renderer (and the video backend that borrowed its device)
    if (m_videoManager) {
        m_videoManager->ShutdownMonitorCompositors();
        m_videoManager->Shutdown();
        m_videoManager.reset();
    }
    if (m_renderer) {
        m_renderer->Shutdown();
    }

    // Reacquire the desktop host if it was lost (e.g., Explorer restart).
    // If still acquired (device loss only), keep the existing host.
    if (!m_desktopHost || !m_desktopHost->IsAcquired()) {
        auto result = m_desktopHost->Acquire();
        if (result.IsError()) {
            REX_LOG_ERROR("Engine", "Failed to reacquire desktop host: {}",
                          result.Error().message);
            return result;
        }
    }

    HWND renderWindow = m_desktopHost->GetRenderWindow();
    int width  = m_desktopHost->GetWidth();
    int height = m_desktopHost->GetHeight();

    m_renderer = std::make_unique<D3D12Renderer>();
    auto result = m_renderer->Initialize(renderWindow, width, height);
    if (result.IsError()) {
        REX_LOG_ERROR("Engine", "Failed to reinitialize renderer: {}",
                      result.Error().message);
        return result;
    }
    m_renderer->SetForceSdr(m_config.forceSdr);

    // Recreate the video backend on the new device.
    m_videoManager = std::make_unique<VideoManager>();
    auto vmResult = m_videoManager->Initialize(m_config, m_renderer->GetDevice().Get());
    if (vmResult.IsError()) {
        REX_LOG_WARN("Engine", "VideoManager re-init failed: {} (video scenes will not play)",
                     vmResult.Error().message);
        m_videoManager.reset();
    }

    // Rebuild per-monitor compositors (device changed) and restore
    // persisted assignments.
    if (m_videoManager) {
        m_videoManager->SetWallpaperHostWindow(
            m_desktopHost->GetWallpaperLayerWindow());
    }
    SetupMonitorCompositors();

    // LoadScene only accepts Initialized/Stopped states; the engine is in
    // Error during recovery, so move to Initialized before the caller
    // (HandleDeviceLoss) reloads the scene.
    SetState(EngineState::Initialized);

    REX_LOG_INFO("Engine", "Renderer reinitialized successfully");
    return shared::Result<void>();
}

// ─── Device-loss recovery ─────────────────────────────────────────────────

shared::Result<void> Engine::HandleDeviceLoss() {
    REX_LOG_INFO("Engine", "Handling device loss...");

    // 1. Save playback state (before any teardown invalidates it).
    const bool wasPlaying = m_playing.load();
    const SceneInfo savedScene = m_currentScene;
    double savedPosition = 0.0;
    if (m_videoManager && m_videoManager->IsOpen()) {
        savedPosition = m_videoManager->GetPosition();
    }

    // 2. Recreate the renderer (new device/swap chain), reacquire the
    //    desktop host if needed, and rebuild the video backend on the new
    //    device. ReinitializeRenderer tears down and recreates
    //    m_videoManager internally — it must not touch the old one.
    auto result = ReinitializeRenderer();
    if (result.IsError()) {
        REX_LOG_ERROR("Engine", "Device-loss recovery failed at renderer: {}",
                      result.Error().message);
        return result;
    }

    // 3. Reload the scene (LoadScene is the single entry point: it opens
    //    the video / texture and applies scene settings).
    if (!savedScene.sourcePath.empty()) {
        auto loadResult = LoadScene(savedScene);
        if (loadResult.IsError()) {
            REX_LOG_WARN("Engine", "Reload scene after recovery failed: {}",
                         loadResult.Error().message);
        }
        // Seek to the saved position for video.
        if (savedScene.type == "video" && m_videoManager &&
            m_videoManager->IsOpen()) {
            m_videoManager->Seek(savedPosition);
        }
    }

    // 4. Restore the playing state.
    if (wasPlaying && !savedScene.sourcePath.empty()) {
        auto playResult = Play();
        if (playResult.IsError()) {
            REX_LOG_WARN("Engine", "Resume after recovery failed: {}",
                         playResult.Error().message);
        }
    }

    REX_LOG_INFO("Engine", "Device-loss recovery complete (resumed at {:.2f}s)",
                 savedPosition);
    return shared::Result<void>();
}


shared::Result<void> Engine::LoadScene(const SceneInfo& scene) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_state != EngineState::Initialized && m_state != EngineState::Stopped) {
        return shared::Result<void>(shared::ErrorCode::InvalidState, "Engine not ready to load scene");
    }

    REX_LOG_INFO("Engine", "Loading scene: {} (type: {}, path: {})",
                 scene.name.c_str(), scene.type.c_str(), scene.sourcePath.c_str());

    m_scenesLoaded.fetch_add(1, std::memory_order_relaxed);
    m_currentScene = scene;
    m_currentPosition = 0.0;

    // Activate the renderer adapter for this scene type. Every later
    // SetProperty call is gated by what the adapter supports, so
    // unsupported properties fail loudly instead of silently no-oping.
    m_adapter = CreateRendererAdapter(scene.type);

    // Load texture / video based on scene type
    if (scene.type == "static" && m_renderer && !scene.sourcePath.empty()) {
        auto texResult = m_renderer->LoadTexture(scene.sourcePath);
        if (texResult.IsError()) {
            REX_LOG_WARN("Engine", "Failed to load texture '{}': {}",
                         scene.sourcePath.c_str(), texResult.Error().message);
        }
        // Per-monitor compositors without an override mirror the main scene.
        if (m_videoManager) {
            m_videoManager->SetSharedStaticWallpaper(scene.sourcePath);
        }
    } else if (scene.type == "video" && !scene.sourcePath.empty()) {
        if (!m_videoManager) {
            REX_LOG_WARN("Engine", "VideoManager not available, video will not play");
        } else {
            auto videoResult = m_videoManager->OpenVideo(scene.sourcePath);
            if (videoResult.IsError()) {
                REX_LOG_WARN("Engine", "Failed to open video '{}': {}",
                             scene.sourcePath.c_str(), videoResult.Error().message);
            } else {
                // Apply scene-level playback settings immediately.
                m_videoManager->SetLoop(scene.loop);
                m_videoManager->SetSpeed(std::clamp(scene.speed, 0.25f, 4.0f));
                // Adaptive resolution: pick the stream that best fits the
                // target monitor. A stream switch may rebuild the pool with
                // new NT handles; the ClearVideoFrame() below drops the
                // renderer's stale slot cache.
                if (m_desktopHost) {
                    m_videoManager->SetTargetResolution(m_desktopHost->GetWidth(),
                                                        m_desktopHost->GetHeight());
                }
            }
        }
    }

    // Push scene-level video adjustments to the renderer (and compositors).
    if (m_renderer) {
        m_renderer->SetVideoAdjustments(m_currentScene.brightness,
                                        m_currentScene.contrast,
                                        m_currentScene.saturation);
    }
    if (m_videoManager) {
        m_videoManager->ApplyVideoAdjustments(m_currentScene.brightness,
                                              m_currentScene.contrast,
                                              m_currentScene.saturation);
        m_videoManager->ApplyFillMode(m_currentScene.fillMode);
    }

    // Push the transform (rotation/flip) to the renderer and compositors.
    ApplyTransform();

    // Drop any GPU frames from a previous video scene.
    if (m_renderer) {
        m_renderer->ClearVideoFrame();
    }

    SetState(EngineState::Stopped);
    return shared::Result<void>();
}

shared::Result<void> Engine::UnloadScene() {
    std::lock_guard<std::mutex> lock(m_mutex);

    REX_LOG_INFO("Engine", "Unloading scene");

    m_scenesReleased.fetch_add(1, std::memory_order_relaxed);

    if (m_videoManager) {
        m_videoManager->Close();
        m_videoManager->ClearVideoFrames();
    }

    if (m_renderer) {
        m_renderer->ClearTexture();
        m_renderer->ClearVideoFrame();
    }

    m_adapter.reset();
    m_playing = false;
    m_currentScene = SceneInfo{};
    m_currentPosition = 0.0;

    SetState(EngineState::Initialized);
    return shared::Result<void>();
}

shared::Result<void> Engine::Play() {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_currentScene.sourcePath.empty()) {
        return shared::Result<void>(shared::ErrorCode::InvalidState, "No scene loaded");
    }

    REX_LOG_INFO("Engine", "Play requested");

    if (m_videoManager && m_currentScene.type == "video") {
        m_videoManager->Play();
    }

    m_playing = true;
    SetState(EngineState::Running);
    m_stopwatch.Start();

    // P16: arm the first present after (re)starting playback.
    m_frameDirty.store(true, std::memory_order_release);
    return shared::Result<void>();
}

shared::Result<void> Engine::Pause() {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_state != EngineState::Running) {
        return shared::Result<void>(shared::ErrorCode::InvalidState, "Not playing");
    }

    REX_LOG_INFO("Engine", "Pause requested");

    if (m_videoManager) {
        m_videoManager->Pause();
    }

    m_playing = false;
    SetState(EngineState::Paused);
    m_stopwatch.Stop();

    return shared::Result<void>();
}

shared::Result<void> Engine::Stop() {
    std::lock_guard<std::mutex> lock(m_mutex);

    REX_LOG_INFO("Engine", "Stop requested");

    if (m_videoManager) {
        m_videoManager->Stop();
    }

    m_playing = false;
    m_currentPosition = 0.0;
    SetState(EngineState::Stopped);
    m_stopwatch.Reset();

    return shared::Result<void>();
}

double Engine::GetPosition() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_playing) {
        return m_stopwatch.ElapsedSeconds();
    }
    return m_currentPosition;
}

double Engine::GetDuration() const {
    if (m_videoManager && m_videoManager->IsOpen()) {
        return m_videoManager->GetDuration();
    }
    return 0.0;
}

shared::Result<void> Engine::AssignMonitor(const std::string& monitorId) {
    std::lock_guard<std::mutex> lock(m_mutex);

    REX_LOG_INFO("Engine", "Assigning monitor: {}", monitorId.c_str());

    auto monitors = EnumerateMonitors();
    for (const auto& m : monitors) {
        if (m.id == monitorId || m.name == monitorId) {
            m_currentMonitor = m;
            REX_LOG_INFO("Engine", "Monitor assigned: {} ({}x{})",
                         m.name.c_str(), m.width, m.height);
            return shared::Result<void>();
        }
    }

    // Fallback: use primary monitor dimensions
    m_currentMonitor.id = monitorId;
    m_currentMonitor.name = monitorId;
    m_currentMonitor.width = GetSystemMetrics(SM_CXSCREEN);
    m_currentMonitor.height = GetSystemMetrics(SM_CYSCREEN);
    m_currentMonitor.isPrimary = true;

    REX_LOG_WARN("Engine", "Monitor '{}' not found, using fallback {}x{}",
                 monitorId.c_str(), m_currentMonitor.width, m_currentMonitor.height);

    return shared::Result<void>();
}

shared::Result<void> Engine::ClearMonitor() {
    std::lock_guard<std::mutex> lock(m_mutex);
    REX_LOG_INFO("Engine", "Clearing monitor");
    m_currentMonitor = MonitorInfo{};
    return shared::Result<void>();
}

std::vector<MonitorInfo> Engine::EnumerateMonitors() const {
    std::vector<MonitorInfo> monitors;

    EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR hMon, HDC, LPRECT rect, LPARAM lParam) -> BOOL {
            auto* vec = reinterpret_cast<std::vector<MonitorInfo>*>(lParam);

            MONITORINFOEXW mi = {};
            mi.cbSize = sizeof(mi);
            if (!GetMonitorInfoW(hMon, &mi)) {
                return TRUE; // skip this monitor, continue enumeration
            }

            MonitorInfo info;
            info.x        = mi.rcMonitor.left;
            info.y        = mi.rcMonitor.top;
            info.width    = mi.rcMonitor.right - mi.rcMonitor.left;
            info.height   = mi.rcMonitor.bottom - mi.rcMonitor.top;
            info.isPrimary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;

            // Use wide-char name for the ID
            char nameBuf[128];
            WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, nameBuf, sizeof(nameBuf), nullptr, nullptr);
            info.id   = nameBuf;
            info.name = info.id;

            // DPI scale — use default 1.0; can be enhanced with GetDpiForMonitor
            UINT dpiX = 96, dpiY = 96;
            info.dpiScale = static_cast<float>(dpiX) / 96.0f;

            // Stable identity across disconnects (EDID descriptor hash).
            info.edidHash = rex::platform::windows::GetMonitorEdidHash(hMon);

            vec->push_back(std::move(info));
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&monitors));

    return monitors;
}

shared::Result<void> Engine::SetFillMode(FillMode mode) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_currentScene.fillMode = mode;
    if (m_renderer) {
        m_renderer->SetFillMode(mode);
    }
    if (m_videoManager) {
        m_videoManager->ApplyFillMode(mode);
    }
    m_frameDirty.store(true, std::memory_order_release);
    REX_LOG_DEBUG("Engine", "Fill mode set to {}", static_cast<int>(mode));
    return shared::Result<void>();
}

shared::Result<void> Engine::SetBrightness(float value) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_currentScene.brightness = std::clamp(value, 0.0f, 2.0f);
    if (m_renderer) {
        m_renderer->SetVideoAdjustments(m_currentScene.brightness,
                                        m_currentScene.contrast,
                                        m_currentScene.saturation);
    }
    if (m_videoManager) {
        m_videoManager->ApplyVideoAdjustments(m_currentScene.brightness,
                                              m_currentScene.contrast,
                                              m_currentScene.saturation);
    }
    m_frameDirty.store(true, std::memory_order_release);
    REX_LOG_DEBUG("Engine", "Brightness set to {:.2f}", m_currentScene.brightness);
    return shared::Result<void>();
}

shared::Result<void> Engine::SetContrast(float value) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_currentScene.contrast = std::clamp(value, 0.0f, 2.0f);
    if (m_renderer) {
        m_renderer->SetVideoAdjustments(m_currentScene.brightness,
                                        m_currentScene.contrast,
                                        m_currentScene.saturation);
    }
    if (m_videoManager) {
        m_videoManager->ApplyVideoAdjustments(m_currentScene.brightness,
                                              m_currentScene.contrast,
                                              m_currentScene.saturation);
    }
    m_frameDirty.store(true, std::memory_order_release);
    REX_LOG_DEBUG("Engine", "Contrast set to {:.2f}", m_currentScene.contrast);
    return shared::Result<void>();
}

shared::Result<void> Engine::SetSaturation(float value) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_currentScene.saturation = std::clamp(value, 0.0f, 2.0f);
    if (m_renderer) {
        m_renderer->SetVideoAdjustments(m_currentScene.brightness,
                                        m_currentScene.contrast,
                                        m_currentScene.saturation);
    }
    if (m_videoManager) {
        m_videoManager->ApplyVideoAdjustments(m_currentScene.brightness,
                                              m_currentScene.contrast,
                                              m_currentScene.saturation);
    }
    m_frameDirty.store(true, std::memory_order_release);
    REX_LOG_DEBUG("Engine", "Saturation set to {:.2f}", m_currentScene.saturation);
    return shared::Result<void>();
}

shared::Result<void> Engine::SetVolume(float value) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_currentScene.volume = std::clamp(value, 0.0f, 1.0f);
    if (m_videoManager) {
        m_videoManager->SetVolume(m_currentScene.volume);
    }
    REX_LOG_DEBUG("Engine", "Volume set to {:.2f}", m_currentScene.volume);
    return shared::Result<void>();
}

shared::Result<void> Engine::SetMute(bool mute) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_muted = mute;
    if (m_videoManager) {
        m_videoManager->SetMute(mute);
    }
    REX_LOG_DEBUG("Engine", "Mute set to {}", mute ? "true" : "false");
    return shared::Result<void>();
}

shared::Result<void> Engine::SetLoop(bool loop) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_loop = loop;
    if (m_videoManager) {
        m_videoManager->SetLoop(loop);
    }
    REX_LOG_DEBUG("Engine", "Loop set to {}", loop ? "true" : "false");
    return shared::Result<void>();
}

shared::Result<void> Engine::SetSpeed(float speed) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_speed = std::clamp(speed, 0.25f, 4.0f);
    if (m_videoManager) {
        m_videoManager->SetSpeed(m_speed.load());
    }
    REX_LOG_DEBUG("Engine", "Speed set to {:.2f}", m_speed.load());
    return shared::Result<void>();
}

// ─── Renderer Adapter ────────────────────────────────────────────────────

shared::Result<void> Engine::SetProperty(const std::string& property, const std::string& value) {
    bool supported = false;
    std::string rendererType;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_adapter) {
            return shared::Result<void>(shared::ErrorCode::InvalidState,
                "No scene loaded; no renderer adapter active");
        }
        supported = m_adapter->Supports(property);
        rendererType = std::string(m_adapter->GetType());
    }

    if (!supported) {
        return shared::Result<void>(shared::ErrorCode::UnsupportedFormat,
            "Property '" + property + "' is not supported by the " + rendererType + " renderer");
    }

    if (property == "fillMode") {
        auto mode = ParseFillMode(value);
        if (!mode) {
            return shared::Result<void>(shared::ErrorCode::InvalidArgument,
                "Unknown fill mode: " + value);
        }
        return SetFillMode(*mode);
    }
    if (property == "volume") {
        return SetVolume(static_cast<float>(std::atof(value.c_str())));
    }
    if (property == "mute") {
        return SetMute(value == "true" || value == "1");
    }
    if (property == "speed") {
        return SetSpeed(static_cast<float>(std::atof(value.c_str())));
    }
    if (property == "loop") {
        return SetLoop(value == "true" || value == "1");
    }
    if (property == "rotation") {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_sceneRotation = static_cast<float>(std::atof(value.c_str()));
        ApplyTransform();
        return shared::Result<void>();
    }
    if (property == "flipH" || property == "flipV") {
        std::lock_guard<std::mutex> lock(m_mutex);
        const bool on = (value == "true" || value == "1");
        if (property == "flipH") m_sceneFlipH = on;
        else                     m_sceneFlipV = on;
        ApplyTransform();
        return shared::Result<void>();
    }

    return shared::Result<void>(shared::ErrorCode::InvalidArgument,
        "Unknown property: " + property);
}

std::vector<std::string> Engine::GetSupportedProperties() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_adapter) return {};
    return m_adapter->GetSupportedProperties();
}

std::string Engine::GetRendererType() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_adapter) return "none";
    return std::string(m_adapter->GetType());
}

shared::Result<void> Engine::EnableAudioReactive(bool enable) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config.enableAudioReactive = enable;
    REX_LOG_INFO("Engine", "Audio-reactive {}", enable ? "enabled" : "disabled");
    return shared::Result<void>();
}

shared::Result<void> Engine::SetAudioSensitivity(float sensitivity) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config.audioSensitivity = std::clamp(sensitivity, 0.0f, 1.0f);
    REX_LOG_DEBUG("Engine", "Audio sensitivity set to {:.2f}", m_config.audioSensitivity);
    return shared::Result<void>();
}

IWallpaperEngine::FrameSurface Engine::GetFrameSurface() const {
    FrameSurface s;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_videoManager || !m_videoManager->IsOpen()) {
        return s;
    }
    const auto last = m_videoManager->GetLastFrame();
    s.available = true;
    s.fenceHandle = reinterpret_cast<uint64_t>(m_videoManager->GetFenceHandle());
    s.adapterLuid = m_videoManager->GetAdapterLuid();
    s.slotCount = m_videoManager->GetSlotCount();
    s.currentSlot = last.slot;
    s.currentFenceValue = last.fenceValue;
    s.width = last.width;
    s.height = last.height;
    s.frameIndex = last.frameIndex;
    s.generation = m_videoManager->GetSurfaceGeneration();
    return s;
}

uint64_t Engine::GetFrameSlotHandle(uint32_t slot) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_videoManager || !m_videoManager->IsOpen()) {
        return 0;
    }
    return reinterpret_cast<uint64_t>(m_videoManager->GetSlotHandle(slot));
}

void Engine::GetTelemetry(EngineTelemetry& out) const {
    out = EngineTelemetry{};

    if (m_videoManager) {
        VideoPlaybackTelemetry vt = m_videoManager->GetTelemetry();
        out.framesDecoded  = vt.framesDecoded;
        out.framesConsumed = vt.framesConsumed;
        out.framesDropped  = vt.framesDropped;
        out.decoderFps     = vt.decoderFps;
        out.renderFps      = vt.renderFps;
        out.ringSlotCount  = vt.ringSlotCount;
        out.ringBytes      = vt.ringBytes;
        out.decodeMsP50    = vt.decodeMsP50;
        out.decodeMsP95    = vt.decodeMsP95;
        out.decodeMsP99    = vt.decodeMsP99;
        out.vpMsP50        = vt.vpMsP50;
        out.vpMsP95        = vt.vpMsP95;
        out.vpMsP99        = vt.vpMsP99;
    }    if (m_renderer) {
        out.textureBytes    = m_renderer->GetTextureBytes();
        out.uploadBytes     = m_renderer->GetUploadBytes();
        out.textureRecreations = m_renderer->GetTextureRecreations();
        out.uploads         = m_renderer->GetUploadCount();
        out.allocatorsPerFrame = m_renderer->GetAllocatorsPerUpload();
    }

    out.videoWidth   = 0;
    out.videoHeight  = 0;
    if (m_videoManager && m_currentScene.type == "video") {
        out.videoWidth  = m_videoManager->GetWidth();
        out.videoHeight = m_videoManager->GetHeight();
    }

    // Adaptive resolution: size of the stream actually being decoded.
    if (m_videoManager && m_videoManager->IsOpen()) {
        out.decodeWidth  = m_videoManager->GetWidth();
        out.decodeHeight = m_videoManager->GetHeight();
    }
    if (m_renderer) {
        out.hdrActive      = m_renderer->IsHDRActive();
        out.displayMaxNits = m_renderer->GetDisplayMaxNits();
    }

    // Adapter topology: decoder (D3D11) vs renderer (D3D12) vs display.
    if (m_videoManager) {
        out.decoderAdapterLuid  = m_videoManager->GetAdapterLuid();
        out.rendererAdapterLuid = m_videoManager->GetD3D12AdapterLuid();
        out.decoderRendererSameAdapter = m_videoManager->IsDecoderRendererSameAdapter();
        const uint64_t displayLuid = QueryDisplayAdapterLuid(m_currentMonitor.id);
        out.rendererDisplaySameAdapter =
            displayLuid != 0 && out.rendererAdapterLuid != 0 &&
            displayLuid == out.rendererAdapterLuid;
    }

    // GPU memory on the renderer's adapter (LOCAL = dedicated VRAM,
    // NON_LOCAL = shared/system-memory segment).
    // NOTE: ID3D12Device does NOT implement IDXGIDevice — resolve the
    // adapter via GetAdapterLuid() + EnumAdapterByLuid instead.
    if (m_renderer && m_videoManager) {
        const uint64_t rendererLuid = m_videoManager->GetD3D12AdapterLuid();
        HRESULT hrFactory = E_FAIL, hrEnum = E_FAIL, hrAs = E_FAIL,
                hrLocal = E_FAIL, hrNonLocal = E_FAIL;
        DXGI_QUERY_VIDEO_MEMORY_INFO local{};
        DXGI_QUERY_VIDEO_MEMORY_INFO nonlocal{};
        if (rendererLuid != 0) {
            Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
            hrFactory = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
            if (SUCCEEDED(hrFactory)) {
                LUID luid{};
                luid.LowPart  = static_cast<DWORD>(rendererLuid & 0xFFFFFFFFull);
                luid.HighPart = static_cast<LONG>(rendererLuid >> 32);
                Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
                hrEnum = factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter));
                if (SUCCEEDED(hrEnum)) {
                    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
                    hrAs = adapter.As(&adapter3);
                    if (SUCCEEDED(hrAs)) {
                        hrLocal    = adapter3->QueryVideoMemoryInfo(
                            0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
                        hrNonLocal = adapter3->QueryVideoMemoryInfo(
                            0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonlocal);
                    }
                }
            }
        }

        // One-shot diagnostic: budget==0 means the chain broke somewhere,
        // which would silently blind every downstream comparison.
        static std::atomic<bool> s_gpuMemWarned{false};
        if (!s_gpuMemWarned.exchange(true) &&
            (rendererLuid == 0 || FAILED(hrFactory) || FAILED(hrEnum) ||
             FAILED(hrAs) || FAILED(hrLocal) || FAILED(hrNonLocal))) {
            REX_LOG_WARN("Engine",
                         "GPU memory query incomplete: luid={:016x} "
                         "factory={:08x} enum={:08x} as3={:08x} "
                         "local={:08x} nonlocal={:08x}",
                         static_cast<uint64_t>(rendererLuid),
                         static_cast<uint32_t>(hrFactory),
                         static_cast<uint32_t>(hrEnum),
                         static_cast<uint32_t>(hrAs),
                         static_cast<uint32_t>(hrLocal),
                         static_cast<uint32_t>(hrNonLocal));
        }
        if (SUCCEEDED(hrLocal)) {
            out.gpuDedicatedBytes = local.CurrentUsage;
            out.gpuBudgetBytes    = local.Budget;
        }
        if (SUCCEEDED(hrNonLocal)) {
            out.gpuSharedBytes = nonlocal.CurrentUsage;
        }
    }

    out.scenesLoaded   = m_scenesLoaded.load(std::memory_order_relaxed);
    out.scenesReleased = m_scenesReleased.load(std::memory_order_relaxed);

    PROCESS_MEMORY_COUNTERS_EX pmc = {};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PPROCESS_MEMORY_COUNTERS>(&pmc), sizeof(pmc))) {
        out.privateBytesMB = static_cast<double>(pmc.PrivateUsage)
                           / (1024.0 * 1024.0);
    }

    out.presentCount = m_presentCount.load(std::memory_order_relaxed);
}

void Engine::DumpStats() const {
    std::lock_guard<std::mutex> lock(m_mutex);

    REX_LOG_INFO("Engine", "=== Engine Stats ===");
    REX_LOG_INFO("Engine", "  State:           {}", static_cast<int>(m_state.load(std::memory_order_acquire)));
    REX_LOG_INFO("Engine", "  Playing:         {}", m_playing ? "true" : "false");
    REX_LOG_INFO("Engine", "  Loop:            {}", m_loop ? "true" : "false");
    REX_LOG_INFO("Engine", "  Speed:           {:.2f}", m_speed.load());
    REX_LOG_INFO("Engine", "  Position:        {:.2f}s", m_currentPosition);
    REX_LOG_INFO("Engine", "  Frame:           {} (time: {:.2f}ms)",
                 m_frameCount, m_frameTime);
    REX_LOG_INFO("Engine", "  Recoveries:      {}", m_recoveryCount);
    REX_LOG_INFO("Engine", "  Device resets:   {}", m_deviceResetCount);
    REX_LOG_INFO("Engine", "  Scene:           {}", m_currentScene.name.c_str());
    REX_LOG_INFO("Engine", "  Monitor:         {} ({}x{})",
                 m_currentMonitor.id.c_str(),
                 m_currentMonitor.width, m_currentMonitor.height);

    if (m_videoManager) {
        auto tel = m_videoManager->GetTelemetry();
        REX_LOG_INFO("Engine", "  ── Playback ──────");
        REX_LOG_INFO("Engine", "  Frames decoded:  {}", tel.framesDecoded);
        REX_LOG_INFO("Engine", "  Frames consumed: {}", tel.framesConsumed);
        REX_LOG_INFO("Engine", "  Frames dropped:  {}", tel.framesDropped);
        REX_LOG_INFO("Engine", "  Decoder FPS:     {:.1f}", tel.decoderFps);
        REX_LOG_INFO("Engine", "  Render FPS:      {:.1f}", tel.renderFps);
    }
}

void Engine::Update(double deltaTime) {
    if (m_state != EngineState::Running) return;

    m_frameCount++;
    m_frameTime = deltaTime;

    if (m_playing) {
        // Read the stopwatch directly — this method runs on the render
        // thread which owns m_stopwatch and m_currentPosition. Calling
        // GetPosition() here would try to acquire m_mutex from the render
        // thread, creating a deadlock with Shutdown() holding m_mutex and
        // waiting for the render thread to exit.
        m_currentPosition = m_stopwatch.ElapsedSeconds();

        if (m_frameCount % 60 == 0) {
            REX_LOG_DEBUG("Engine", "Frame {}: {:.2f} s, {:.2f} ms",
                          m_frameCount, m_currentPosition, deltaTime);
        }
    }
}

void Engine::CheckDisplayChanges() {
    auto monitors = EnumerateMonitors();
    int count = static_cast<int>(monitors.size());

    int primaryW = 0, primaryH = 0;
    for (const auto& m : monitors) {
        if (m.isPrimary) {
            primaryW = m.width;
            primaryH = m.height;
            break;
        }
    }

    bool changed = (count != m_lastMonitorCount)
                || (primaryW != m_lastPrimaryWidth)
                || (primaryH != m_lastPrimaryHeight);

    if (!changed) return;

    REX_LOG_INFO("Engine", "Display topology changed: {} monitors (was {})",
                 count, m_lastMonitorCount);

    m_lastMonitorCount = count;
    m_lastPrimaryWidth  = primaryW;
    m_lastPrimaryHeight = primaryH;

    // Multi-monitor mode: reconcile compositors with the new topology
    // (add connected, remove disconnected, update moved/resized).
    if (IsMultiMonitorMode() && m_videoManager) {
        auto active = m_videoManager->GetActiveMonitors();

        for (const auto& a : active) {
            bool found = false;
            for (const auto& m : monitors) {
                if (m.id != a.id) continue;
                found = true;
                if (m.width != a.width || m.height != a.height ||
                    m.x != a.x || m.y != a.y) {
                    m_videoManager->UpdateMonitor(m);
                }
                break;
            }
            if (!found) {
                auto rm = m_videoManager->RemoveMonitor(a.id);
                if (rm.IsError()) {
                    REX_LOG_WARN("Engine", "RemoveMonitor failed: {}",
                                 rm.Error().message);
                }
            }
        }

        for (const auto& m : monitors) {
            bool found = false;
            for (const auto& a : active) {
                if (a.id == m.id) { found = true; break; }
            }
            if (!found) {
                auto am = m_videoManager->AddMonitor(m);
                if (am.IsError()) {
                    REX_LOG_WARN("Engine", "AddMonitor failed: {}",
                                 am.Error().message);
                }
            }
        }
    }

    // Update the assigned monitor dimensions if it was the primary
    if (m_currentMonitor.isPrimary) {
        m_currentMonitor.width  = primaryW;
        m_currentMonitor.height = primaryH;
    }

    // Notify renderer of new dimensions
    if (m_renderer && m_desktopHost) {
        m_renderer->Resize(
            m_desktopHost->GetWidth(),
            m_desktopHost->GetHeight());
    }
}

void Engine::SetState(EngineState state) {
    m_state.store(state, std::memory_order_release);
    REX_LOG_DEBUG("Engine", "State changed to {}", static_cast<int>(state));
}

void Engine::ApplyTransform() {
    if (m_renderer) {
        m_renderer->SetRotation(m_sceneRotation);
        m_renderer->SetFlip(m_sceneFlipH, m_sceneFlipV);
    }
    if (m_videoManager) {
        m_videoManager->ApplyTransform(m_sceneRotation, m_sceneFlipH,
                                       m_sceneFlipV);
    }
}

void Engine::CheckFullScreenPause() {
    if (!m_config.pauseOnFullscreen) return;

    // A foreground window that covers >=95% of its monitor's bounds is
    // treated as full-screen. Monitor-aware: a game on a secondary monitor
    // must not be judged against the whole virtual desktop.
    bool fullScreen = false;
    HWND fg = GetForegroundWindow();
    if (fg && IsWindowVisible(fg) && !IsIconic(fg)) {
        RECT fgRect;
        if (GetWindowRect(fg, &fgRect) && fgRect.right > fgRect.left &&
            fgRect.bottom > fgRect.top) {
            MONITORINFO mi = {};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST),
                                &mi)) {
                RECT inter;
                IntersectRect(&inter, &fgRect, &mi.rcMonitor);
                const int iw = inter.right - inter.left;
                const int ih = inter.bottom - inter.top;
                const int mw = mi.rcMonitor.right - mi.rcMonitor.left;
                const int mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
                fullScreen = (static_cast<int64_t>(iw) * ih * 100 >=
                              static_cast<int64_t>(mw) * mh * 95);
            }
        }
    }

    // IMPORTANT: Do NOT call Pause()/Play() directly here. Both methods
    // acquire m_mutex, and this function runs on the render thread. If
    // Shutdown() is concurrently holding m_mutex and waiting for the render
    // thread to exit, calling Pause()/Play() here would deadlock.
    // Instead, post an atomic signal that the render loop services before
    // the next GPU submit — fully lock-free on the render thread.
    if (fullScreen && !m_fullScreenPaused && m_state == EngineState::Running) {
        m_fullScreenPaused = true;
        REX_LOG_INFO("Engine", "Full-screen app detected — requesting wallpaper pause");
        m_fullScreenRequest.store(1, std::memory_order_release);
    } else if (!fullScreen && m_fullScreenPaused) {
        m_fullScreenPaused = false;
        REX_LOG_INFO("Engine", "Full-screen app closed — requesting wallpaper resume");
        m_fullScreenRequest.store(-1, std::memory_order_release);
    }
}

void Engine::RenderLoop() {
    using namespace std::chrono;

    REX_LOG_INFO("Engine", "Render loop started");
    REX_LOG_INFO("Engine", "Desktop host: type={} renderWindow={} {}×{} valid={}",
                 m_desktopHost ? std::string(m_desktopHost->GetHostType()) : "none",
                 m_desktopHost ? fmt::ptr(m_desktopHost->GetRenderWindow()) : nullptr,
                 m_desktopHost ? m_desktopHost->GetWidth() : 0,
                 m_desktopHost ? m_desktopHost->GetHeight() : 0,
                 m_desktopHost ? (m_desktopHost->IsValid() ? "yes" : "no") : "N/A");

    while (m_running) {
        auto frameStart = high_resolution_clock::now();

        // ── Auto-pause behind full-screen apps (throttled) ──────────────
        if (m_frameCount % 30 == 0) {
            CheckFullScreenPause();
        }

        // ── Service fullscreen pause/resume request (lock-free) ─────────
        // CheckFullScreenPause() posts 1 (pause) or -1 (resume) to avoid
        // calling the locking Pause()/Play() APIs from the render thread.
        // We service the request here, at the top of the frame, before any
        // GPU work begins.
        if (const int req = m_fullScreenRequest.exchange(0, std::memory_order_acq_rel)) {
            if (req == 1 && m_state == EngineState::Running) {
                // Pause: stop video + mark engine paused (no m_mutex needed;
                // render thread exclusively owns these during normal operation)
                if (m_videoManager) m_videoManager->Pause();
                m_playing = false;
                m_stopwatch.Stop();
                SetState(EngineState::Paused);
                REX_LOG_INFO("Engine", "Full-screen pause applied");
            } else if (req == -1 && m_state == EngineState::Paused) {
                // Resume: restart video + mark engine running
                if (m_videoManager) m_videoManager->Play();
                m_playing = true;
                m_stopwatch.Start();
                SetState(EngineState::Running);
                REX_LOG_INFO("Engine", "Full-screen resume applied");
            }
        }


        // ── Periodically check for display topology changes ─────────────
        if (m_frameCount % 120 == 0) {
            CheckDisplayChanges();
        }

        // ── Desktop host / shell restart recovery ─────────────────────
        // The flag is set from the desktop host's monitor thread; we
        // handle it here on the render thread so there's no mutex
        // contention.
        if (m_recoveryNeeded.exchange(false)) {
            REX_LOG_INFO("Engine", "WorkerW recovery requested, reinitializing...");
            SetState(EngineState::Error);
            m_recoveryCount++;

            auto recoveryResult = HandleDeviceLoss();
            if (recoveryResult.IsSuccess()) {
                m_frameCount = 0;
            }
            continue;
        }

        // ── Detect device loss ─────────────────────────────────────────
        if (m_renderer && m_renderer->IsDeviceRemoved()) {
            REX_LOG_ERROR("Engine", "Device lost, attempting recovery...");
            SetState(EngineState::Error);
            m_deviceResetCount++;
            m_recoveryCount++;

            auto recoveryResult = HandleDeviceLoss();
            if (recoveryResult.IsSuccess()) {
                m_frameCount = 0;
            }
            continue;
        }

        if (m_renderer && m_renderer->IsInitialized()) {
            // ── P16 dirty-frame gate ────────────────────────────────────
            // Poll the scheduler EVERY wake (lock-free SPSC, pointer stays
            // valid until the next Tick), but only Begin/Draw/Present when
            // content actually changed. Without this, the loop re-rendered
            // identical frames at ~refresh rate — burning 36-55% GPU when
            // the decoder ran slower than the display.
            const VideoFrame* pending = nullptr;
            if (m_state == EngineState::Running && m_videoManager
                && m_videoManager->IsOpen()
                && m_currentScene.type == "video") {
                pending = m_videoManager->Tick(GetPosition());
                if (pending && !(pending->valid)) pending = nullptr;
            }

            if (!pending && !m_frameDirty.load(std::memory_order_acquire)) {
                m_frameCount++;              // keep %30/%120 housekeeping alive
                PaceLoop(frameStart);
                continue;
            }

            m_renderer->BeginFrame();

            if (pending && m_renderer) {
                m_renderer->DrawVideoFrame(*pending,
                                           m_videoManager->GetSharedFence());
                if (IsMultiMonitorMode()) {
                    // Same decoded frame to every monitor compositor.
                    m_videoManager->PresentToAllMonitors(*pending);
                }
                // New pixels fully handled this pass — disarm any setter-
                // sourced dirty so we don't double-present.
                m_frameDirty.store(false, std::memory_order_release);
            }

            // ── Clear color (static dark until texture is loaded) ──────
            // Applied once per texture-less state; re-presenting identical
            // dark frames is exactly what P16 exists to prevent.
            if (m_state == EngineState::Running
                && !m_renderer->HasTexture()) {
                if (!m_darkCleared) {
                    m_renderer->SetClearColor(0.02f, 0.02f, 0.02f);
                    m_darkCleared = true;
                }
            } else {
                m_darkCleared = false;
            }

            m_renderer->RenderFrame();
            m_renderer->EndFrame();

            auto frameEnd = high_resolution_clock::now();
            double deltaMs = duration<double, std::milli>(
                frameEnd - frameStart).count();
            Update(deltaMs / 1000.0);

            m_presentCount.fetch_add(1, std::memory_order_relaxed);
            PaceLoop(frameStart);
        } else {
            // Renderer not ready — wait before retrying
            std::this_thread::sleep_for(milliseconds(16));
        }
    }

    REX_LOG_INFO("Engine", "Render loop ended");
}

// ─── P16 content-aware pacing ──────────────────────────────────────────────
//
// Sleeps until the next expected frame. Target interval is the SLOWER of
// maxFps and the decoder's measured cadence, floored at ~15 Hz so pause/
// resume/unload/SET_* commands stay responsive.

void Engine::PaceLoop(
    std::chrono::high_resolution_clock::time_point frameStart) {
    // P16 final form: wake at the maxFps rate unconditionally. Skipped
    // wakes cost only a lock-free scheduler peek (~microseconds); renders
    // and Presents stay gated on actual new content. Chasing the
    // decoder's instantaneous fps proved unstable under software-decode
    // load swings and caused scheduler coalescing (false "dropped" counts).
    const double targetIntervalMs =
        m_config.maxFps > 0 ? std::max(1000.0 / m_config.maxFps, 4.0) : 16.7;
    const auto deadline = frameStart +
        std::chrono::duration_cast<std::chrono::high_resolution_clock::duration>(
            std::chrono::duration<double, std::milli>(targetIntervalMs));
    std::this_thread::sleep_until(deadline);
}

// ─── Per-monitor wallpapers (Phase 5) ─────────────────────────────────────

void Engine::SetupMonitorCompositors() {
    if (!m_videoManager || !IsMultiMonitorMode()) return;

    for (const auto& info : EnumerateMonitors()) {
        auto result = m_videoManager->AddMonitor(info);
        if (result.IsError()) {
            REX_LOG_WARN("Engine", "Failed to add compositor for monitor '{}': {}",
                         info.id.c_str(), result.Error().message);
        }
    }
    m_videoManager->RestoreMonitorAssignments();
}

shared::Result<void> Engine::SetPerMonitorWallpaper(const std::string& monitorId,
                                                    const std::string& path) {
    if (!m_videoManager) {
        return shared::Result<void>(shared::ErrorCode::NotInitialized,
                                    "video manager not initialized");
    }
    return m_videoManager->SetPerMonitorWallpaper(monitorId, path);
}

void Engine::SetSettingsService(
    std::shared_ptr<rex::settings::ISettingsService> settings) {
    if (m_videoManager) {
        m_videoManager->SetSettingsService(std::move(settings));
    }
}

} // namespace rex::wallpaper

``

## src/wallpaper/engine/Engine.hpp

``cpp
#pragma once

#include <rex/wallpaper/IWallpaperEngine.hpp>
#include <shared/Logger.hpp>
#include <shared/UUID.hpp>
#include <shared/Stopwatch.hpp>

#include <atomic>
#include <mutex>
#include <memory>
#include <thread>

#include "platform/windows/IDesktopHost.hpp"
#include "graphics/renderer/D3D12Renderer.hpp"
#include "wallpaper/video/VideoManager.hpp"
#include "modules/Settings/interfaces/ISettingsService.hpp"
#include <rex/wallpaper/IRendererAdapter.hpp>

namespace rex::wallpaper {

class Engine : public IWallpaperEngine {
public:
    Engine();
    ~Engine();

    shared::Result<void> Initialize(const WallpaperConfig& config) override;
    shared::Result<void> Shutdown() override;
    EngineState GetState() const override { return m_state.load(std::memory_order_acquire); }

    shared::Result<void> LoadScene(const SceneInfo& scene) override;
    shared::Result<void> UnloadScene() override;
    SceneInfo GetCurrentScene() const override { return m_currentScene; }

    shared::Result<void> Play() override;
    shared::Result<void> Pause() override;
    shared::Result<void> Stop() override;
    bool IsPlaying() const override { return m_playing; }
    double GetPosition() const override;
    double GetDuration() const override;

    shared::Result<void> AssignMonitor(const std::string& monitorId) override;
    shared::Result<void> ClearMonitor() override;
    MonitorInfo GetCurrentMonitor() const override { return m_currentMonitor; }
    std::vector<MonitorInfo> EnumerateMonitors() const override;

    shared::Result<void> SetFillMode(FillMode mode) override;
    shared::Result<void> SetBrightness(float value) override;
    shared::Result<void> SetContrast(float value) override;
    shared::Result<void> SetSaturation(float value) override;
    shared::Result<void> SetVolume(float value) override;
    shared::Result<void> SetMute(bool mute) override;
    shared::Result<void> SetLoop(bool loop) override;
    shared::Result<void> SetSpeed(float speed) override;

    shared::Result<void> SetProperty(const std::string& property, const std::string& value) override;
    std::vector<std::string> GetSupportedProperties() const override;
    std::string GetRendererType() const override;

    shared::Result<void> EnableAudioReactive(bool enable) override;
    shared::Result<void> SetAudioSensitivity(float sensitivity) override;

    void DumpStats() const override;
    FrameSurface GetFrameSurface() const override;
    uint64_t GetFrameSlotHandle(uint32_t slot) const override;
    void GetTelemetry(EngineTelemetry& out) const override;
    shared::UUID GetSessionId() const override { return m_sessionId; }

    // ─── Per-monitor wallpapers (Phase 5) ────────────────────────────────
    shared::Result<void> SetPerMonitorWallpaper(const std::string& monitorId,
                                                const std::string& path) override;
    void SetSettingsService(
        std::shared_ptr<rex::settings::ISettingsService> settings) override;

private:
    void Update(double deltaTime);
    void SetState(EngineState state);
    void RenderLoop();
    /// P16: sleep until the next expected frame (decoder cadence clamped
    /// by maxFps, floor ~15Hz so pause/resume/unload stay responsive).
    void PaceLoop(std::chrono::high_resolution_clock::time_point frameStart);
    shared::Result<void> ReinitializeRenderer();
    shared::Result<void> HandleDeviceLoss();
    void CheckDisplayChanges();
    /// Auto-pause/resume while a full-screen app owns the foreground.
    void CheckFullScreenPause();
    /// Push the current rotation/flip to the main renderer and the
    /// per-monitor compositors.
    void ApplyTransform();
    /// True when the active host provides a wallpaper layer (MultiMonitor).
    bool IsMultiMonitorMode() const {
        return m_desktopHost && m_desktopHost->GetWallpaperLayerWindow() != nullptr;
    }
    /// Add a compositor per monitor + restore persisted assignments.
    void SetupMonitorCompositors();

    // Atomic so the render thread can safely read it while the command
    // thread swaps scenes (UnloadScene/LoadScene) under the mutex.
    std::atomic<EngineState> m_state{EngineState::Uninitialized};
    std::atomic<bool> m_playing{false};
    std::atomic<bool> m_loop{true};
    std::atomic<float> m_speed{1.0f};
    bool m_muted = false;  // guarded by m_mutex

    SceneInfo m_currentScene;
    MonitorInfo m_currentMonitor;
    WallpaperConfig m_config;

    // ─── Transform state (rotation / flip) ──────────────────────────────
    float m_sceneRotation = 0.0f;
    bool  m_sceneFlipH = false;
    bool  m_sceneFlipV = false;

    shared::UUID m_sessionId;

    mutable std::mutex m_mutex;
    shared::Stopwatch m_stopwatch;
    double m_currentPosition = 0.0;

    // ─── Telemetry ─────────────────────────────────────────────────────
    double m_frameTime = 0.0;
    long long m_frameCount = 0;
    uint32_t m_recoveryCount = 0;
    uint32_t m_deviceResetCount = 0;
    int m_lastMonitorCount = 0;
    int m_lastPrimaryWidth = 0;
    int m_lastPrimaryHeight = 0;

    // Scene lifecycle counters (for the memory-retention diagnostics)
    std::atomic<uint64_t> m_scenesLoaded{0};
    std::atomic<uint64_t> m_scenesReleased{0};

    std::unique_ptr<rex::platform::windows::IDesktopHost> m_desktopHost;
    std::unique_ptr<D3D12Renderer> m_renderer;
    std::unique_ptr<VideoManager> m_videoManager;
    std::unique_ptr<IRendererAdapter> m_adapter;
    std::thread m_renderThread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_recoveryNeeded{false};

    // ─── Dirty-frame gating & present telemetry (P16) ──────────────────
    // Set by frame adoption on the render thread and by IPC-thread
    // property setters; consumed (and cleared) only in RenderLoop.
    std::atomic<bool> m_frameDirty{false};
    std::atomic<uint64_t> m_presentCount{0};
    // Render-thread only: dark clear applied once per texture-less state.
    bool m_darkCleared = false;
    // ─── Full-screen auto-pause (render-thread internal) ──────────────
    /// Posted by CheckFullScreenPause() so the render loop can service
    /// it without calling the locking Pause()/Play() APIs from inside
    /// the render thread (which would deadlock against Shutdown()).
    ///   0 = no request pending
    ///   1 = pause requested
    ///  -1 = resume requested
    std::atomic<int> m_fullScreenRequest{0};

    /// Auto-pause state (render-thread only): true while we paused the
    /// wallpaper behind a full-screen app, so we resume only what we
    /// paused ourselves (never a user-initiated pause).
    bool m_fullScreenPaused = false;
};

} // namespace rex::wallpaper

``

## src/wallpaper/engine/EngineFactory.cpp

``cpp
#include <rex/wallpaper/IWallpaperEngine.hpp>
#include "wallpaper/engine/Engine.hpp"
#include <memory>

namespace rex::wallpaper {

std::unique_ptr<IWallpaperEngine> CreateWallpaperEngine() {
    return std::make_unique<Engine>();
}

} // namespace rex::wallpaper

``

## src/wallpaper/decoder/FfmpegVideoDecoder.cpp

``cpp
#include "wallpaper/decoder/FfmpegVideoDecoder.hpp"

#include <shared/Logger.hpp>

#include <d3d11_1.h>
#include <dxgi1_2.h>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <chrono>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <vector>

namespace rex::wallpaper {

using Clock = std::chrono::steady_clock;
using ms = std::chrono::duration<double, std::milli>;

namespace {

constexpr const char* kModule = "FfmpegVideoDecoder";

std::string Hex(HRESULT hr) {
    char buf[16] = {};
    std::snprintf(buf, sizeof(buf), "%08lX", static_cast<unsigned long>(hr));
    return buf;
}

} // namespace

FfmpegVideoDecoder::FfmpegVideoDecoder() = default;

FfmpegVideoDecoder::~FfmpegVideoDecoder() {
    Close();
}

shared::Result<void> FfmpegVideoDecoder::Open(std::string_view path, bool useHardware,
                                              ID3D11DeviceContext* context11,
                                              HANDLE fenceHandle) {
    if (m_open.load(std::memory_order_acquire)) {
        return shared::Result<void>(shared::ErrorCode::AlreadyInitialized, "decoder already open");
    }
    if (path.empty()) {
        return shared::Result<void>(shared::ErrorCode::InvalidArgument, "empty path");
    }
    if (!context11) {
        return shared::Result<void>(shared::ErrorCode::InvalidArgument,
                                    "a D3D11 context is required (same adapter as D3D12)");
    }

    // The immediate context is only obtainable from D3D11CreateDevice's
    // out-param (ID3D11Device does NOT QI into the context); recover the
    // device from it instead. ID3D11DeviceChild::GetDevice returns void.
    m_context = context11;
    m_context->GetDevice(&m_device11);
    if (!m_device11) {
        m_context.Reset();
        return shared::Result<void>(shared::ErrorCode::HardwareNotSupported,
                                    "cannot get D3D11 device from context");
    }
    ID3D11VideoDevice* rawVideoDevice = nullptr;
    ID3D11VideoContext* rawVideoContext = nullptr;
    ID3D11VideoContext1* rawVideoContext1 = nullptr;
    ID3D11DeviceContext4* rawContext4 = nullptr;
    HRESULT hVid  = m_device11->QueryInterface(IID_PPV_ARGS(&rawVideoDevice));
    HRESULT hVCtx = context11->QueryInterface(IID_PPV_ARGS(&rawVideoContext));
    HRESULT hVC1  = context11->QueryInterface(IID_PPV_ARGS(&rawVideoContext1));
    HRESULT hCtx4 = context11->QueryInterface(IID_PPV_ARGS(&rawContext4));
    if (FAILED(hVid) || FAILED(hVCtx) || FAILED(hVC1) || FAILED(hCtx4)) {
        m_context.Reset();
        m_device11.Reset();
        return shared::Result<void>(
            shared::ErrorCode::HardwareNotSupported,
            "D3D11 device lacks video support (vid=0x" + Hex(hVid) + ", vctx=0x" +
                Hex(hVCtx) + ", vctx1=0x" + Hex(hVC1) + ", ctx4=0x" + Hex(hCtx4) + ")");
    }
    m_videoDevice.Attach(rawVideoDevice);
    m_videoContext.Attach(rawVideoContext);
    m_videoContext1.Attach(rawVideoContext1);
    m_context4.Attach(rawContext4);

    if (fenceHandle) {
        Microsoft::WRL::ComPtr<ID3D11Device5> d5;
        if (FAILED(m_device11.As(&d5)) ||
            FAILED(d5->OpenSharedFence(fenceHandle, IID_PPV_ARGS(&m_fence11)))) {
            m_device11.Reset();
            return shared::Result<void>(shared::ErrorCode::DeviceLost,
                                        "failed to open shared fence (OpenSharedFence)");
        }
    } else {
        m_device11.Reset();
        return shared::Result<void>(shared::ErrorCode::InvalidArgument,
                                    "a shared fence handle is required");
    }

    // â”€â”€ FFmpeg: open input â”€â”€
    av_log_set_level(AV_LOG_ERROR);
    m_fmtCtx = nullptr;
    if (avformat_open_input(&m_fmtCtx, std::string(path).c_str(), nullptr, nullptr) < 0) {
        m_fence11.Reset();
        m_device11.Reset();
        return shared::Result<void>(shared::ErrorCode::FileNotFound, "cannot open input file");
    }
    if (avformat_find_stream_info(m_fmtCtx, nullptr) < 0) {
        Close();
        return shared::Result<void>(shared::ErrorCode::InvalidFormat, "cannot read stream info");
    }
    m_videoStreamIndex = av_find_best_stream(m_fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (m_videoStreamIndex < 0) {
        Close();
        return shared::Result<void>(shared::ErrorCode::InvalidFormat, "no video stream found");
    }
    m_stream = m_fmtCtx->streams[m_videoStreamIndex];
    m_duration = m_fmtCtx->duration != AV_NOPTS_VALUE
                     ? static_cast<double>(m_fmtCtx->duration) / AV_TIME_BASE
                     : 0.0;
    if (m_stream->r_frame_rate.den > 0) {
        m_frameRate = static_cast<double>(m_stream->r_frame_rate.num) /
                      m_stream->r_frame_rate.den;
    }
    if (m_frameRate <= 0.0) {
        m_frameRate = 30.0;
    }
    m_width  = m_stream->codecpar->width;
    m_height = m_stream->codecpar->height;

    const AVCodec* codec = avcodec_find_decoder(m_stream->codecpar->codec_id);
    if (!codec) {
        Close();
        return shared::Result<void>(shared::ErrorCode::UnsupportedFormat, "no decoder for codec");
    }
    m_decCtx = avcodec_alloc_context3(codec);
    if (!m_decCtx || avcodec_parameters_to_context(m_decCtx, m_stream->codecpar) < 0) {
        Close();
        return shared::Result<void>(shared::ErrorCode::OutOfMemory, "cannot create codec context");
    }
    m_decCtx->thread_count = 1;  // deterministic sequential decode (wallpaper-style)
    m_decCtx->pkt_timebase = m_stream->time_base;

    // â”€â”€ Hardware decode (D3D11VA on our D3D11 device) â”€â”€
    m_hardware = false;
    if (useHardware) {
        AVBufferRef* hwDevRef = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (hwDevRef) {
            AVHWDeviceContext* hwDev =
                reinterpret_cast<AVHWDeviceContext*>(hwDevRef->data);
            AVD3D11VADeviceContext* hwctx =
                reinterpret_cast<AVD3D11VADeviceContext*>(hwDev->hwctx);
            hwctx->device = m_device11.Get();
            hwctx->BindFlags = D3D11_BIND_SHADER_RESOURCE;  // FFmpeg adds BIND_DECODER
            hwctx->MiscFlags = D3D11_RESOURCE_MISC_SHARED |
                               D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
            if (av_hwdevice_ctx_init(hwDevRef) >= 0) {
                m_decCtx->hw_device_ctx = av_buffer_ref(hwDevRef);
                m_hwDevCtx = hwDevRef;
                if (avcodec_open2(m_decCtx, codec, nullptr) < 0) {
                    REX_LOG_WARN(kModule, "hardware decode open failed, retrying software");
                    avcodec_free_context(&m_decCtx);
                    m_decCtx = nullptr;
                    av_buffer_unref(&m_hwDevCtx);
                    m_hwDevCtx = nullptr;
                } else {
                    m_hardware = true;
                }
            } else {
                av_buffer_unref(&hwDevRef);
            }
        }
        if (!m_hardware) {
            REX_LOG_WARN(kModule, "D3D11VA unavailable, falling back to software decode");
        }
    }

    // â”€â”€ Software fallback â”€â”€
    if (!m_hardware) {
        if (avcodec_open2(m_decCtx, codec, nullptr) < 0) {
            Close();
            return shared::Result<void>(shared::ErrorCode::UnsupportedFormat,
                                        "software decoder open failed");
        }
        m_sws = sws_getContext(m_width, m_height,
                               static_cast<AVPixelFormat>(m_stream->codecpar->format),
                               m_width, m_height, AV_PIX_FMT_BGRA,
                               SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!m_sws) {
            Close();
            return shared::Result<void>(shared::ErrorCode::OutOfMemory,
                                        "cannot create swscale context");
        }
    }

    m_frame = av_frame_alloc();
    m_swFrame = av_frame_alloc();
    m_pkt = av_packet_alloc();
    if (!m_frame || !m_swFrame || !m_pkt) {
        Close();
        return shared::Result<void>(shared::ErrorCode::OutOfMemory, "frame/packet alloc failed");
    }

    m_eof = false;
    m_pktPending = false;
    m_open.store(true, std::memory_order_release);

    // P13 diagnostics: exactly which codec/profile/pixel path/adapter is
    // in play. Answers "why is decode slow?" without a debugger.
    {
        const char* hwaccel = m_hardware ? "d3d11va" : "software";
        const char* pixfmt = av_get_pix_fmt_name(
            m_hardware ? m_decCtx->sw_pix_fmt
                       : static_cast<AVPixelFormat>(m_decCtx->pix_fmt));
        uint64_t luid = 0;
        if (m_device11) {
            Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDev;
            Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
            DXGI_ADAPTER_DESC desc{};
            if (SUCCEEDED(m_device11.As(&dxgiDev)) &&
                SUCCEEDED(dxgiDev->GetAdapter(&adapter)) &&
                SUCCEEDED(adapter->GetDesc(&desc))) {
                luid = static_cast<uint64_t>(desc.AdapterLuid.LowPart) |
                       (static_cast<uint64_t>(desc.AdapterLuid.HighPart) << 32);
            }
        }
        REX_LOG_INFO(kModule,
                     "decoder: codec={} profile={} hwaccel={} pixelFormat={} "
                     "adapterLUID=0x{:016x}",
                     avcodec_get_name(m_decCtx->codec_id),
                     m_decCtx->profile, hwaccel,
                     pixfmt ? pixfmt : "unknown", luid);
        REX_LOG_INFO(kModule, "opened '{}' {}x{} @{:.2f} fps ({} decode)", path,
                     m_width, m_height, m_frameRate,
                     m_hardware ? "hardware" : "software");
    }
    return shared::Result<void>();
}

void FfmpegVideoDecoder::Close() {
    m_open.store(false, std::memory_order_release);

    if (m_decCtx) {
        avcodec_free_context(&m_decCtx);
        m_decCtx = nullptr;
    }
    av_buffer_unref(&m_hwDevCtx);
    if (m_fmtCtx) {
        avformat_close_input(&m_fmtCtx);
        m_fmtCtx = nullptr;
    }
    av_frame_free(&m_frame);
    av_frame_free(&m_swFrame);
    av_packet_free(&m_pkt);
    if (m_sws) {
        sws_freeContext(m_sws);
        m_sws = nullptr;
    }

    m_videoStreamIndex = -1;
    m_stream = nullptr;
    m_pktPending = false;
    m_hardware = false;
    m_eof = false;
    m_duration = 0.0;
    m_frameRate = 30.0;
    m_width = 0;
    m_height = 0;

    for (HANDLE h : m_poolHandles) {
        if (h) CloseHandle(h);
    }
    m_poolHandles.clear();
    m_poolTex.clear();
    m_staging.Reset();
    m_enum.Reset();
    m_processor.Reset();
    m_poolW = 0;
    m_poolH = 0;
    m_poolNext = 0;
    m_poolHdr = false;
    m_hdrContent = false;
    m_fenceValue = 0;
    m_frameIndex = 0;
    m_colorInfo = VideoColorInfo{};
    m_sws16bit = false;

    m_fence11.Reset();
    m_context4.Reset();
    m_videoContext1.Reset();
    m_videoContext.Reset();
    m_videoDevice.Reset();
    m_context.Reset();
    m_device11.Reset();
}

bool FfmpegVideoDecoder::Seek(double seconds) {
    if (!m_open.load(std::memory_order_acquire)) return false;
    return SeekInternal(seconds);
}

bool FfmpegVideoDecoder::Flush() {
    if (!m_decCtx) return false;
    avcodec_flush_buffers(m_decCtx);
    m_eof = false;
    return true;
}

bool FfmpegVideoDecoder::SeekInternal(double seconds) {
    if (!m_fmtCtx || !m_stream) return false;
    const int64_t ts = av_rescale_q(static_cast<int64_t>(seconds * AV_TIME_BASE),
                                    AV_TIME_BASE_Q, m_stream->time_base);
    int ret = av_seek_frame(m_fmtCtx, m_videoStreamIndex, ts, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        ret = av_seek_frame(m_fmtCtx, -1, ts, AVSEEK_FLAG_BACKWARD);
    }
    if (ret >= 0) {
        avcodec_flush_buffers(m_decCtx);
        m_eof = false;
        m_pktPending = false;
        return true;
    }
    REX_LOG_WARN(kModule, "seek to {:.2f}s failed (av_seek_frame {})", seconds, ret);
    return false;
}

void FfmpegVideoDecoder::SelectStreamByResolution(int targetWidth, int targetHeight) {
    if (!m_fmtCtx || !m_open.load(std::memory_order_acquire)) return;

    const int64_t targetPixels =
        static_cast<int64_t>(targetWidth) * static_cast<int64_t>(targetHeight);

    // Pass 1: smallest stream that fits within the target resolution.
    int bestIdx = -1;
    int64_t bestPixels = INT64_MAX;
    for (size_t i = 0; i < m_fmtCtx->nb_streams; ++i) {
        AVStream* s = m_fmtCtx->streams[i];
        if (!s || !s->codecpar || s->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) continue;
        const int64_t pixels =
            static_cast<int64_t>(s->codecpar->width) * s->codecpar->height;
        if (s->codecpar->width <= 0 || s->codecpar->height <= 0) continue;
        if (pixels <= targetPixels && pixels < bestPixels) {
            bestIdx = static_cast<int>(i);
            bestPixels = pixels;
        }
    }
    // Pass 2: nothing fits â€” take the smallest stream available.
    if (bestIdx == -1) {
        bestPixels = INT64_MAX;
        for (size_t i = 0; i < m_fmtCtx->nb_streams; ++i) {
            AVStream* s = m_fmtCtx->streams[i];
            if (!s || !s->codecpar || s->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) continue;
            const int64_t pixels =
                static_cast<int64_t>(s->codecpar->width) * s->codecpar->height;
            if (s->codecpar->width <= 0 || s->codecpar->height <= 0) continue;
            if (pixels < bestPixels) {
                bestIdx = static_cast<int>(i);
                bestPixels = pixels;
            }
        }
    }
    if (bestIdx < 0 || bestIdx == m_videoStreamIndex) return;

    AVStream* newStream = m_fmtCtx->streams[bestIdx];
    const AVCodec* codec = avcodec_find_decoder(newStream->codecpar->codec_id);
    if (!codec) return;

    // â”€â”€ Re-open the codec on the new stream â”€â”€
    avcodec_free_context(&m_decCtx);
    m_decCtx = avcodec_alloc_context3(codec);
    if (!m_decCtx || avcodec_parameters_to_context(m_decCtx, newStream->codecpar) < 0) {
        avcodec_free_context(&m_decCtx);
        m_decCtx = nullptr;
        REX_LOG_ERROR(kModule, "stream switch failed: cannot create codec context");
        return;
    }
    m_decCtx->thread_count = 1;
    m_decCtx->pkt_timebase = newStream->time_base;

    // Keep hardware decode when it was active (device context must be
    // attached before avcodec_open2); fall back to software on failure.
    bool hwOk = false;
    if (m_hardware && m_hwDevCtx) {
        m_decCtx->hw_device_ctx = av_buffer_ref(m_hwDevCtx);
        if (avcodec_open2(m_decCtx, codec, nullptr) < 0) {
            REX_LOG_WARN(kModule, "stream switch: hardware open failed, retrying software");
            avcodec_free_context(&m_decCtx);
            m_decCtx = avcodec_alloc_context3(codec);
            if (!m_decCtx ||
                avcodec_parameters_to_context(m_decCtx, newStream->codecpar) < 0) {
                avcodec_free_context(&m_decCtx);
                m_decCtx = nullptr;
                return;
            }
            m_decCtx->thread_count = 1;
            m_decCtx->pkt_timebase = newStream->time_base;
        } else {
            hwOk = true;
        }
    }
    if (!hwOk && avcodec_open2(m_decCtx, codec, nullptr) < 0) {
        avcodec_free_context(&m_decCtx);
        m_decCtx = nullptr;
        REX_LOG_ERROR(kModule, "stream switch failed: cannot open decoder");
        return;
    }

    // Software fallback needs a swscale context sized for the new stream.
    if (!hwOk) {
        sws_freeContext(m_sws);
        m_sws = sws_getContext(newStream->codecpar->width, newStream->codecpar->height,
                               static_cast<AVPixelFormat>(newStream->codecpar->format),
                               newStream->codecpar->width, newStream->codecpar->height,
                               AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!m_sws) {
            avcodec_free_context(&m_decCtx);
            m_decCtx = nullptr;
            REX_LOG_ERROR(kModule, "stream switch failed: cannot create swscale context");
            return;
        }
    }

    // Adopt the new stream: timebase for pts conversion, stream properties,
    // and fresh decode state. The pool is rebuilt lazily by EnsurePool when
    // the next decoded frame has different dimensions.
    m_videoStreamIndex = bestIdx;
    m_stream = newStream;
    m_hardware = hwOk;
    m_duration = m_fmtCtx->duration != AV_NOPTS_VALUE
                     ? static_cast<double>(m_fmtCtx->duration) / AV_TIME_BASE
                     : 0.0;
    if (m_stream->r_frame_rate.den > 0) {
        m_frameRate = static_cast<double>(m_stream->r_frame_rate.num) /
                      m_stream->r_frame_rate.den;
    }
    if (m_frameRate <= 0.0) {
        m_frameRate = 30.0;
    }
    m_width  = newStream->codecpar->width;
    m_height = newStream->codecpar->height;
    m_eof = false;
    m_pktPending = false;
    REX_LOG_INFO(kModule, "stream switched: {}x{} @{:.2f} fps ({} decode)", m_width,
                 m_height, m_frameRate, m_hardware ? "hardware" : "software");
}

// â”€â”€â”€ Colorimetry â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€

VideoColorInfo FfmpegVideoDecoder::ResolveColorInfo() const {
    VideoColorInfo info;
    info.srcWidth  = m_frame ? m_frame->width  : m_width;
    info.srcHeight = m_frame ? m_frame->height : m_height;

    // Fallback chain: frame properties â†’ codec context (derived from the
    // stream at avcodec_open2) â†’ reasonable defaults.
    AVColorSpace spc = m_frame ? m_frame->colorspace : m_decCtx->colorspace;
    AVColorTransferCharacteristic trc = m_frame ? m_frame->color_trc : m_decCtx->color_trc;
    AVColorRange rng = m_frame ? m_frame->color_range : m_decCtx->color_range;
    if (spc == AVCOL_SPC_UNSPECIFIED) spc = m_decCtx->colorspace;
    if (trc == AVCOL_TRC_UNSPECIFIED) trc = m_decCtx->color_trc;
    if (rng == AVCOL_RANGE_UNSPECIFIED) rng = m_decCtx->color_range;

    switch (spc) {
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M:
            info.yuvMatrix = VideoColorInfo::Matrix::BT601;
            break;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:
            info.yuvMatrix = VideoColorInfo::Matrix::BT2020;
            break;
        default:
            info.yuvMatrix = VideoColorInfo::Matrix::BT709;
            break;
    }

    switch (trc) {
        case AVCOL_TRC_SMPTE2084:
            info.transfer = VideoColorInfo::Transfer::PQ;
            break;
        case AVCOL_TRC_ARIB_STD_B67:
            info.transfer = VideoColorInfo::Transfer::HLG;
            break;
        default:
            info.transfer = VideoColorInfo::Transfer::SDR;
            break;
    }

    info.fullRange = (rng == AVCOL_RANGE_JPEG);

    // HDR static metadata: frame side data wins over stream-level
    // metadata. FFmpeg 7+ removed av_stream_get_side_data and the
    // AVCodecParameters mastering/CLL fields; stream metadata now lives in
    // codecpar->coded_side_data (read via av_packet_side_data_get).
    double maxLum = 0.0, minLum = 0.0, maxCll = 0.0, maxFall = 0.0;
    if (m_frame) {
        if (const auto* sd = av_frame_get_side_data(
                m_frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA)) {
            const auto* md = reinterpret_cast<const AVMasteringDisplayMetadata*>(sd->data);
            if (md->has_luminance) {
                maxLum = av_q2d(md->max_luminance);
                minLum = av_q2d(md->min_luminance);
            }
        }
        if (const auto* sd = av_frame_get_side_data(
                m_frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL)) {
            const auto* cl = reinterpret_cast<const AVContentLightMetadata*>(sd->data);
            maxCll = cl->MaxCLL;
            maxFall = cl->MaxFALL;
        }
    }
    if (m_stream && m_stream->codecpar) {
        if (maxLum <= 0.0) {
            if (const auto* sd = av_packet_side_data_get(
                    m_stream->codecpar->coded_side_data,
                    m_stream->codecpar->nb_coded_side_data,
                    AV_PKT_DATA_MASTERING_DISPLAY_METADATA)) {
                const auto* md =
                    reinterpret_cast<const AVMasteringDisplayMetadata*>(sd->data);
                if (md->has_luminance) {
                    maxLum = av_q2d(md->max_luminance);
                    minLum = av_q2d(md->min_luminance);
                }
            }
        }
        if (maxCll <= 0.0) {
            if (const auto* sd = av_packet_side_data_get(
                    m_stream->codecpar->coded_side_data,
                    m_stream->codecpar->nb_coded_side_data,
                    AV_PKT_DATA_CONTENT_LIGHT_LEVEL)) {
                const auto* cl =
                    reinterpret_cast<const AVContentLightMetadata*>(sd->data);
                maxCll = cl->MaxCLL;
                maxFall = cl->MaxFALL;
            }
        }
    }

    const double defaultPeak = (info.transfer == VideoColorInfo::Transfer::HLG) ? 1000.0 : 1000.0;
    if (info.transfer != VideoColorInfo::Transfer::SDR) {
        info.masteringDisplayMaxLuminance = static_cast<float>(maxLum > 0.0 ? maxLum : defaultPeak);
        info.masteringDisplayMinLuminance = static_cast<float>(minLum);
        info.maxContentLightLevel      = static_cast<float>(maxCll > 0.0 ? maxCll : defaultPeak);
        info.maxFrameAverageLightLevel = static_cast<float>(maxFall > 0.0 ? maxFall : defaultPeak * 0.4f);
    }
    return info;
}

bool FfmpegVideoDecoder::DecodeFrame(VideoFrame& outFrame) {
    if (!m_open.load(std::memory_order_acquire) || !m_decCtx) return false;

    const auto t0 = Clock::now();
    while (true) {
        const int r = avcodec_receive_frame(m_decCtx, m_frame);
        if (r == 0) {
            // Resolve colorimetry of this frame (frame side data may change
            // mid-stream). Drives pool format, VP color spaces and the
            // renderer's shader constants.
            m_colorInfo = ResolveColorInfo();
            m_hdrContent = m_colorInfo.transfer != VideoColorInfo::Transfer::SDR;
            outFrame.colorInfo = m_colorInfo;
            const bool ok = ConvertAndSignal(outFrame);
            const double decodeMs = ms(Clock::now() - t0).count();
            m_lastDecodeMs.store(decodeMs, std::memory_order_relaxed);
            m_decodeMsSum.fetch_add(decodeMs, std::memory_order_relaxed);
            // P13 ring sample (decode = receive_frame â†’ pool publish).
            m_decodeMsRing[m_decodeMsCount % kTimingRingSize] =
                static_cast<float>(decodeMs);
            ++m_decodeMsCount;
            if (ok) {
                m_framesDecoded.fetch_add(1, std::memory_order_relaxed);
            }
            return ok;
        }
        if (r == AVERROR(EAGAIN)) {
            // decoder needs more packets; fall through to read one
        } else if (r == AVERROR_EOF) {
            if (m_eof) {
                if (m_looping.load(std::memory_order_relaxed) && SeekInternal(0.0)) {
                    continue;
                }
                return false;
            }
            m_eof = true;
            avcodec_send_packet(m_decCtx, nullptr);  // flush remaining frames
            continue;
        } else {
            REX_LOG_ERROR(kModule, "avcodec_receive_frame failed ({})", r);
            return false;
        }

        if (!m_pktPending) {
            const int pr = av_read_frame(m_fmtCtx, m_pkt);
            if (pr == AVERROR_EOF) {
                if (!m_eof) {
                    m_eof = true;
                    avcodec_send_packet(m_decCtx, nullptr);  // flush remaining frames
                }
                continue;
            }
            if (pr < 0) {
                REX_LOG_ERROR(kModule, "av_read_frame failed ({})", pr);
                return false;
            }
            if (m_pkt->stream_index != m_videoStreamIndex) {
                av_packet_unref(m_pkt);
                continue;
            }
            m_pktPending = true;
        }
        const int s = avcodec_send_packet(m_decCtx, m_pkt);
        av_packet_unref(m_pkt);
        m_pktPending = false;
        if (s == 0 || s == AVERROR(EAGAIN)) {
            continue;
        }
        REX_LOG_ERROR(kModule, "avcodec_send_packet failed ({})", s);
        return false;
    }
}

HANDLE FfmpegVideoDecoder::GetSlotHandle(UINT slot) const {
    if (!m_open.load(std::memory_order_acquire) || slot >= m_poolHandles.size()) {
        return nullptr;
    }
    return m_poolHandles[slot];
}

UINT64 FfmpegVideoDecoder::GetSlotFence(UINT slot) const {
    if (slot >= kPoolSize) return 0;
    return m_slotFence[slot].load(std::memory_order_acquire);
}

// P13: percentile over a fixed ring of the most recent samples.
static double PercentileOfRing(const float* ring, size_t count, double p) {
    if (count == 0) return 0.0;
    const size_t n = std::min(count, FfmpegVideoDecoder::kTimingRingSize);
    std::vector<float> sorted(ring, ring + static_cast<long>(n));
    const size_t idx = static_cast<size_t>(
        std::clamp(p, 0.0, 1.0) * static_cast<double>(n - 1));
    std::nth_element(sorted.begin(),
                     sorted.begin() + static_cast<long>(idx),
                     sorted.end());
    return static_cast<double>(sorted[idx]);
}

DecoderDiagnostics FfmpegVideoDecoder::GetDiagnostics() const {
    DecoderDiagnostics d;
    d.framesDecoded = m_framesDecoded.load(std::memory_order_relaxed);
    d.cpuToGpuCopies = m_cpuToGpuCopies.load(std::memory_order_relaxed);
    d.lastDecodeMs = m_lastDecodeMs.load(std::memory_order_relaxed);
    const double sum = m_decodeMsSum.load(std::memory_order_relaxed);
    const uint64_t n = d.framesDecoded;
    d.averageDecodeMs = n > 0 ? sum / static_cast<double>(n) : 0.0;
    d.hardwareAccelerated = m_hardware;
    d.eof = m_eof;
    d.decodeMsP50 = PercentileOfRing(m_decodeMsRing.data(), m_decodeMsCount, 0.50);
    d.decodeMsP95 = PercentileOfRing(m_decodeMsRing.data(), m_decodeMsCount, 0.95);
    d.decodeMsP99 = PercentileOfRing(m_decodeMsRing.data(), m_decodeMsCount, 0.99);
    d.vpMsP50     = PercentileOfRing(m_vpMsRing.data(),     m_vpMsCount,     0.50);
    d.vpMsP95     = PercentileOfRing(m_vpMsRing.data(),     m_vpMsCount,     0.95);
    d.vpMsP99     = PercentileOfRing(m_vpMsRing.data(),     m_vpMsCount,     0.99);
    return d;
}

bool FfmpegVideoDecoder::EnsurePool(UINT width, UINT height) {
    if (m_poolW == width && m_poolH == height &&
        m_poolTex.size() == kPoolSize && m_processor &&
        m_poolHdr == m_hdrContent) {
        return true;
    }

    for (HANDLE h : m_poolHandles) {
        if (h) CloseHandle(h);
    }
    m_poolHandles.clear();
    m_poolTex.clear();
    m_poolTex.resize(kPoolSize);
    m_poolHandles.resize(kPoolSize, nullptr);
    m_poolW = width;
    m_poolH = height;
    m_poolNext = 0;
    m_poolHdr = m_hdrContent;

    REX_LOG_INFO(kModule, "pool rebuilt: {} slots, {}x{}, {} (bounded)",
                 kPoolSize, width, height,
                 m_hdrContent ? "P010 (GPU HDR path)" : "NV12 (SDR bypass VP)");

    // SDR: NV12 pool (bypass VP, shader does YUV→RGB).
    // HDR + HW: P010 pool (GPU-resident, shader does BT.2020 PQ decode).
    // HDR + SW: RGBA16_UNORM pool (software swscale outputs RGBA64).
    const DXGI_FORMAT poolFormat =
        m_hdrContent ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;

    for (size_t i = 0; i < m_poolTex.size(); ++i) {
        D3D11_TEXTURE2D_DESC pd = {};
        pd.Width = width;
        pd.Height = height;
        pd.MipLevels = 1;
        pd.ArraySize = 1;
        pd.Format = poolFormat;
        pd.SampleDesc.Count = 1;
        pd.Usage = D3D11_USAGE_DEFAULT;
        // P010 (multi-planar) doesn't support RENDER_TARGET; NV12 and RGBA16 do.
        pd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (poolFormat == DXGI_FORMAT_NV12 ||
            poolFormat == DXGI_FORMAT_R16G16B16A16_FLOAT ||
            poolFormat == DXGI_FORMAT_R16G16B16A16_UNORM) {
            pd.BindFlags |= D3D11_BIND_RENDER_TARGET;
        }
        pd.MiscFlags = D3D11_RESOURCE_MISC_SHARED |
                       D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        if (FAILED(m_device11->CreateTexture2D(&pd, nullptr, &m_poolTex[i]))) {
            return false;
        }
        Microsoft::WRL::ComPtr<IDXGIResource1> res1;
        if (SUCCEEDED(m_poolTex[i].As(&res1))) {
            res1->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr,
                                     &m_poolHandles[i]);
        }
    }

    // Hardware YUV->RGB video processor (NV12 decode -> BGRA pool).
    m_enum.Reset();
    m_processor.Reset();
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd = {};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputFrameRate = { 30, 1 };
    cd.InputWidth = width;
    cd.InputHeight = height;
    cd.OutputFrameRate = { 30, 1 };
    cd.OutputWidth = width;
    cd.OutputHeight = height;
    cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    HRESULT hr = m_videoDevice->CreateVideoProcessorEnumerator(&cd, &m_enum);
    if (SUCCEEDED(hr)) {
        hr = m_videoDevice->CreateVideoProcessor(m_enum.Get(), 0, &m_processor);
    }
    if (FAILED(hr)) {
        REX_LOG_ERROR(kModule, "create video processor failed (0x{:08x})", hr);
        return false;
    }

    // Software-upload staging texture.
    m_staging.Reset();
    D3D11_TEXTURE2D_DESC sd = {};
    sd.Width = width;
    sd.Height = height;
    sd.MipLevels = 1;
    sd.ArraySize = 1;
    sd.Format = poolFormat;
    sd.SampleDesc.Count = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(m_device11->CreateTexture2D(&sd, nullptr, &m_staging))) {
        REX_LOG_ERROR(kModule, "create staging texture failed");
        return false;
    }
    return true;
}

bool FfmpegVideoDecoder::ConvertAndSignal(VideoFrame& outFrame) {
    // True software decode (no D3D11VA): always use software path.
    if (m_frame->format != AV_PIX_FMT_D3D11) {
        return ConvertSoftware(outFrame);
    }

    // HW-decoded frame: log surface format once per session.
    auto* tex11 = reinterpret_cast<ID3D11Texture2D*>(m_frame->data[0]);
    const UINT arraySlice = static_cast<UINT>(
        reinterpret_cast<intptr_t>(m_frame->data[1]));
    D3D11_TEXTURE2D_DESC desc11 = {};
    tex11->GetDesc(&desc11);

    static bool s_loggedFormat = false;
    if (!s_loggedFormat) {
        const char* fmtName = "unknown";
        switch (desc11.Format) {
            case DXGI_FORMAT_NV12:       fmtName = "NV12"; break;
            case DXGI_FORMAT_P010:       fmtName = "P010"; break;
            case DXGI_FORMAT_YUY2:       fmtName = "YUY2"; break;
            case DXGI_FORMAT_AYUV:       fmtName = "AYUV"; break;
            case DXGI_FORMAT_R8G8B8A8_UNORM: fmtName = "BGRA"; break;
            case DXGI_FORMAT_R16G16B16A16_FLOAT: fmtName = "RGBA16F"; break;
        }
        const char* decodePath = m_hdrContent ? "HW_P010" : "HW_NV12";
        REX_LOG_INFO(kModule, "decoder surface: {}x{} fmt={} (0x{:x}) "
                     "hdr={} decodePath={}",
                     desc11.Width, desc11.Height, fmtName,
                     static_cast<unsigned>(desc11.Format), m_hdrContent,
                     decodePath);
        s_loggedFormat = true;
    }

    if (!EnsurePool(desc11.Width, desc11.Height)) {
        return false;
    }

    const UINT slot = m_poolNext;
    m_poolNext = (m_poolNext + 1) % kPoolSize;

    // GPU-resident path: copy decoder surface directly into pool.
    // SDR: NV12 → NV12 pool, shader does BT.709 YUV→RGB.
    // HDR: P010 → P010 pool, shader does BT.2020 Y′CbCr → PQ EOTF → RGB.
    const UINT srcSubresource = arraySlice;
    D3D11_BOX srcBox = { 0, 0, 0,
                         desc11.Width, desc11.Height, 1 };
    m_context->CopySubresourceRegion(m_poolTex[slot].Get(), 0, 0, 0, 0,
                                      tex11, srcSubresource, &srcBox);

    ++m_fenceValue;
    m_slotFence[slot].store(m_fenceValue, std::memory_order_release);
    m_context4->Signal(m_fence11.Get(), m_fenceValue);
    m_context->Flush();

    outFrame.sharedHandle = m_poolHandles[slot];
    outFrame.slot = slot;
    outFrame.fenceValue = m_fenceValue;
    outFrame.pts = m_frame->best_effort_timestamp != AV_NOPTS_VALUE
                       ? static_cast<double>(av_rescale_q(m_frame->best_effort_timestamp,
                                                          m_stream->time_base,
                                                          AV_TIME_BASE_Q)) / AV_TIME_BASE
                       : 0.0;
    outFrame.frameIndex = m_frameIndex++;
    outFrame.width = m_frame->width;
    outFrame.height = m_frame->height;
    outFrame.valid = true;
    return true;
}

bool FfmpegVideoDecoder::ConvertSoftware(VideoFrame& outFrame) {
    // Software fallback: swscale to BGRA (SDR) / RGBA64 (HDR), staging
    // upload, CopyResource into the pool slot (CPU round-trip).
    // For HW-decoded HDR: downloads D3D11 frame to CPU first via
    // av_hwframe_transfer_data, then swscale on the software frame.
    if (!m_loggedSwFallback) {
        m_loggedSwFallback = true;
        REX_LOG_WARN(kModule, "software frame conversion (CPU upload) "
                     "active decodePath=SW_RGBA16");
    }
    if (!m_swFrame || !m_stream) return false;

    // If the decoded frame is D3D11 (HW accel), download to CPU first.
    AVFrame* srcFrame = m_frame;
    AVFrame* dlFrame = nullptr;
    const bool isD3d11 = (m_frame->format == AV_PIX_FMT_D3D11);
    if (isD3d11) {
        dlFrame = av_frame_alloc();
        if (!dlFrame) return false;
        dlFrame->format = AV_PIX_FMT_NONE;
        if (av_hwframe_transfer_data(dlFrame, m_frame, 0) < 0) {
            REX_LOG_ERROR(kModule, "hwframe download failed for HDR frame");
            av_frame_free(&dlFrame);
            return false;
        }
        srcFrame = dlFrame;
    }

    const auto frameFormat = static_cast<AVPixelFormat>(srcFrame->format);
    const AVPixelFormat swFormat = m_hdrContent ? AV_PIX_FMT_RGBA64 : AV_PIX_FMT_BGRA;
    if (!m_sws ||
        frameFormat != static_cast<AVPixelFormat>(m_stream->codecpar->format) ||
        m_sws16bit != m_hdrContent) {
        if (m_sws) {
            sws_freeContext(m_sws);
        }
        m_sws = sws_getContext(srcFrame->width, srcFrame->height, frameFormat,
                               srcFrame->width, srcFrame->height, swFormat,
                               SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!m_sws) {
            REX_LOG_ERROR(kModule, "swscale context creation failed");
            if (dlFrame) av_frame_free(&dlFrame);
            return false;
        }
        m_sws16bit = m_hdrContent;
        // Tell swscale the source YUV range/matrix so limited/full and
        // 601/709/2020 sources convert correctly.
        // For HDR (BT.2020 PQ): use BT.2020 as BOTH source and dest
        // so swscale does YCbCr→RGB with BT.2020 matrix but NO gamut
        // conversion. The shader handles BT.2020→BT.709 in linear space
        // after PQ decode.
        const int srcRange = m_colorInfo.fullRange ? 1 : 0;
        const int srcCsp = (m_colorInfo.yuvMatrix == VideoColorInfo::Matrix::BT601)
                               ? SWS_CS_SMPTE170M
                               : (m_colorInfo.yuvMatrix == VideoColorInfo::Matrix::BT2020)
                                     ? SWS_CS_BT2020
                                     : SWS_CS_ITU709;
        const int dstCsp = m_hdrContent ? srcCsp : SWS_CS_DEFAULT;
        const int dstRange = m_hdrContent ? 1 : 0;  // HDR: full range RGB output
        sws_setColorspaceDetails(m_sws, sws_getCoefficients(srcCsp), srcRange,
                                 sws_getCoefficients(dstCsp), dstRange,
                                 0, 1 << 16, 1 << 16);
    }

    if (!EnsurePool(static_cast<UINT>(srcFrame->width),
                    static_cast<UINT>(srcFrame->height))) {
        if (dlFrame) av_frame_free(&dlFrame);
        return false;
    }
    const UINT slot = m_poolNext;
    m_poolNext = (m_poolNext + 1) % kPoolSize;

    av_frame_unref(m_swFrame);
    m_swFrame->format = swFormat;
    m_swFrame->width = srcFrame->width;
    m_swFrame->height = srcFrame->height;
    if (av_frame_get_buffer(m_swFrame, 32) < 0) {
        if (dlFrame) av_frame_free(&dlFrame);
        return false;
    }
    sws_scale(m_sws, srcFrame->data, srcFrame->linesize, 0, srcFrame->height,
              m_swFrame->data, m_swFrame->linesize);

    // Free the download frame now that swscale is done.
    if (dlFrame) av_frame_free(&dlFrame);

    D3D11_MAPPED_SUBRESOURCE map = {};
    if (FAILED(m_context->Map(m_staging.Get(), 0, D3D11_MAP_WRITE, 0, &map))) {
        return false;
    }
    const uint8_t* src = m_swFrame->data[0];
    const int srcPitch = m_swFrame->linesize[0];
    const UINT rowBytes =
        static_cast<UINT>(m_frame->width) * (m_hdrContent ? 8u : 4u);
    uint8_t* dst = static_cast<uint8_t*>(map.pData);
    for (UINT y = 0; y < static_cast<UINT>(m_frame->height); ++y) {
        std::memcpy(dst + static_cast<size_t>(y) * map.RowPitch,
                    src + static_cast<size_t>(y) * srcPitch, rowBytes);
    }
    m_context->Unmap(m_staging.Get(), 0);
    m_context->CopyResource(m_poolTex[slot].Get(), m_staging.Get());

    ++m_fenceValue;
    m_slotFence[slot].store(m_fenceValue, std::memory_order_release);
    m_context4->Signal(m_fence11.Get(), m_fenceValue);
    m_context->Flush();

    m_cpuToGpuCopies.fetch_add(1, std::memory_order_relaxed);

    outFrame.sharedHandle = m_poolHandles[slot];
    outFrame.slot = slot;
    outFrame.fenceValue = m_fenceValue;
    outFrame.pts = m_frame->best_effort_timestamp != AV_NOPTS_VALUE
                       ? static_cast<double>(av_rescale_q(m_frame->best_effort_timestamp,
                                                          m_stream->time_base,
                                                          AV_TIME_BASE_Q)) / AV_TIME_BASE
                       : 0.0;
    outFrame.frameIndex = m_frameIndex++;
    outFrame.width = m_frame->width;
    outFrame.height = m_frame->height;
    outFrame.valid = true;
    return true;
}

} // namespace rex::wallpaper

``

## src/wallpaper/decoder/FfmpegVideoDecoder.hpp

``cpp
#pragma once

#include <shared/Result.hpp>

#include <windows.h>
#include <d3d11_4.h>
#include <wrl/client.h>

#include <atomic>
#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
}

struct SwsContext;  // libswscale (software conversion)

namespace rex::wallpaper {

// ─── VideoColorInfo ─────────────────────────────────────────────────────
//
// Colorimetry of the decoded source, carried on every frame so the renderer
// can do the right thing without probing the decoder: SDR (BT.601/709,
// no EOTF), PQ (ST.2084, BT.2020) or HLG (ARIB STD-B67, BT.2020). HDR
// static metadata (Mastering Display + Content Light Level) is lifted from
// the stream's side data when present.

struct VideoColorInfo {
    enum class Matrix { BT601, BT709, BT2020 };
    enum class Transfer { SDR, PQ, HLG };

    Matrix yuvMatrix = Matrix::BT709;
    Transfer transfer = Transfer::SDR;
    bool fullRange = false;
    int srcWidth = 0;
    int srcHeight = 0;

    // HDR static metadata (in cd/m²; defaults match SDR expectations).
    float masteringDisplayMinLuminance = 0.0f;
    float masteringDisplayMaxLuminance = 1000.0f;
    float maxContentLightLevel = 1000.0f;
    float maxFrameAverageLightLevel = 400.0f;
};

// ─── VideoFrame ─────────────────────────────────────────────────────────
//
// GPU-backed decoded frame. The content lives in the decoder's private
// ring of shareable BGRA textures (pool slot `slot`). The D3D12 consumer
// opens each slot's NT shared handle once (FfmpegVideoDecoder::GetSlotHandle)
// and waits on the slot's D3D12 fence value (GetSlotFence) before sampling;
// both are exposed through VideoManager. Fence values are monotonic, so
// waiting on the latest value of a slot is always race-free.

struct VideoFrame {
    HANDLE   sharedHandle = nullptr;  // NT handle of the pool slot (stable per Open)
    UINT     slot       = 0;      // pool slot index (stable for process lifetime)
    UINT64   fenceValue = 0;      // fence value signaled after the blt into slot
    double   pts        = 0.0;    // presentation timestamp in seconds
    uint64_t frameIndex = 0;      // monotonically increasing decode order
    UINT     width      = 0;
    UINT     height     = 0;
    bool     valid      = false;
    VideoColorInfo colorInfo;     // source colorimetry (HDR metadata when any)
};

// ─── DecoderDiagnostics ─────────────────────────────────────────────────

struct DecoderDiagnostics {
    uint64_t framesDecoded    = 0;  // frames produced by DecodeFrame
    uint64_t gpuToCpuCopies   = 0;  // staging downloads (0 on the GPU path)
    uint64_t cpuToGpuCopies   = 0;  // software-upload conversions
    double   decodeFps        = 0.0;
    double   averageDecodeMs  = 0.0;
    double   lastDecodeMs     = 0.0;
    // P13 per-stage percentiles over the last 128 frames. "decode" spans
    // avcodec_receive_frame through pool publish; "vp" is VideoProcessorBlt
    // alone (including its HDR-fallback retry when one occurs).
    double   decodeMsP50      = 0.0;
    double   decodeMsP95      = 0.0;
    double   decodeMsP99      = 0.0;
    double   vpMsP50          = 0.0;
    double   vpMsP95          = 0.0;
    double   vpMsP99          = 0.0;
    uint32_t recoveryCount    = 0;
    uint32_t deviceResets     = 0;
    bool     hardwareAccelerated = false;
    bool     eof              = false;
};

// ─── FfmpegVideoDecoder ─────────────────────────────────────────────────
//
// FFmpeg decode (D3D11VA hardware, libswscale software fallback) that
// converts every frame into a private ring of shareable BGRA textures and
// signals a shared D3D11 fence after each conversion.
//
// FFmpeg's decode textures carry D3D11_BIND_DECODER, which D3D11 refuses to
// share (CreateSharedHandle -> DXGI_ERROR_INVALID_CALL even with
// SHARED | SHARED_NTHANDLE), so each frame is copied into our own pool via a
// hardware video processor (GPU-only, no CPU round-trip) and those pool
// textures are what get shared with D3D12.
//
// Threading: DecodeFrame() must be called from exactly one thread (the
// decode thread). All other members are safe to read concurrently.

class FfmpegVideoDecoder {
 public:
    /// P13 timing-ring capacity (also used by diagnostics percentile math).
    static constexpr size_t kTimingRingSize = 128;

    FfmpegVideoDecoder();
    ~FfmpegVideoDecoder();

    FfmpegVideoDecoder(const FfmpegVideoDecoder&) = delete;
    FfmpegVideoDecoder& operator=(const FfmpegVideoDecoder&) = delete;

    // context11 must be the immediate context of a D3D11 device on the
    // same adapter as the D3D12 device (created by VideoManager with
    // D3D11_CREATE_DEVICE_VIDEO_SUPPORT | BGRA_SUPPORT). The device is
    // recovered from the context (ID3D11Device does not QI into the
    // context). fenceHandle: NT handle of a D3D12-created shared fence,
    // opened here as an ID3D11Fence and signaled after every conversion.
    shared::Result<void> Open(std::string_view path, bool useHardware,
                              ID3D11DeviceContext* context11, HANDLE fenceHandle);
    void Close();
    bool IsOpen() const { return m_open.load(std::memory_order_acquire); }

    /// Decodes one frame. Returns false on EOF (or error); on success
    /// outFrame describes the pool slot the frame was converted into.
    bool DecodeFrame(VideoFrame& outFrame);

    void SetLoop(bool loop) { m_looping.store(loop, std::memory_order_relaxed); }
    bool GetLoop() const { return m_looping.load(std::memory_order_relaxed); }
    bool Seek(double seconds);
    bool Flush();

    /// Picks the video stream closest to (but not above) the target
    /// resolution; if none fits, the smallest stream. Re-opens the codec
    /// on the chosen stream, keeping the current decode session alive.
    /// Must not run concurrently with DecodeFrame() (caller serializes).
    void SelectStreamByResolution(int targetWidth, int targetHeight);

    double GetDuration()  const { return m_duration; }
    double GetFrameRate() const { return m_frameRate; }
    int    GetWidth()     const { return m_width; }
    int    GetHeight()    const { return m_height; }

    // Shared-surface contract for the D3D12 renderer.
    HANDLE GetSlotHandle(UINT slot) const;
    UINT64 GetSlotFence(UINT slot) const;

    /// Colorimetry of the stream as last resolved from a decoded frame.
    const VideoColorInfo& GetColorInfo() const { return m_colorInfo; }

    DecoderDiagnostics GetDiagnostics() const;

private:
    bool SeekInternal(double seconds);
    VideoColorInfo ResolveColorInfo() const;
    bool EnsurePool(UINT width, UINT height);
    bool ConvertAndSignal(VideoFrame& outFrame);
    bool ConvertSoftware(VideoFrame& outFrame);

    // ─── FFmpeg state ───────────────────────────────────────────────────
    AVFormatContext* m_fmtCtx      = nullptr;
    AVCodecContext*  m_decCtx      = nullptr;
    AVBufferRef*     m_hwDevCtx    = nullptr;
    AVFrame*         m_frame       = nullptr;
    AVFrame*         m_swFrame     = nullptr;
    AVPacket*        m_pkt         = nullptr;
    SwsContext*      m_sws         = nullptr;
    int  m_videoStreamIndex        = -1;
    AVStream* m_stream             = nullptr;
    bool m_pktPending              = false;
    bool m_hardware                = false;
    bool m_eof                     = false;
    std::atomic<bool> m_looping{true};
    bool m_loggedSwFallback        = false;

    double m_duration  = 0.0;
    double m_frameRate = 30.0;
    int    m_width     = 0;
    int    m_height    = 0;

    // ─── D3D11 state ────────────────────────────────────────────────────
    Microsoft::WRL::ComPtr<ID3D11Device>         m_device11;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>  m_context;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> m_context4;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice>    m_videoDevice;
    Microsoft::WRL::ComPtr<ID3D11VideoContext>   m_videoContext;
    Microsoft::WRL::ComPtr<ID3D11VideoContext1>  m_videoContext1;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> m_enum;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> m_processor;
    Microsoft::WRL::ComPtr<ID3D11Fence>          m_fence11;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>      m_staging;

    // ─── Shareable BGRA pool ────────────────────────────────────────────
    static constexpr UINT kPoolSize = 8;
    std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> m_poolTex;
    std::vector<HANDLE> m_poolHandles;
    std::atomic<UINT64> m_slotFence[kPoolSize]{};
    UINT m_poolW = 0;
    UINT m_poolH = 0;
    UINT m_poolNext = 0;
    bool m_poolHdr = false;   // pool format is 16-bit float (HDR sources)
    bool m_hdrContent = false; // current stream carries PQ/HLG transfer
    UINT64 m_fenceValue = 0;
    UINT64 m_frameIndex = 0;
    std::atomic<bool> m_open{false};

    // ─── Colorimetry (last resolved from a decoded frame) ───────────────
    VideoColorInfo m_colorInfo;
    bool m_sws16bit = false;  // swscale target is RGBA64 (HDR software path)

    // ─── Telemetry ──────────────────────────────────────────────────────
    mutable std::atomic<uint64_t> m_framesDecoded{0};
    mutable std::atomic<uint64_t> m_cpuToGpuCopies{0};
    mutable std::atomic<double>   m_decodeMsSum{0.0};
    mutable std::atomic<double>   m_lastDecodeMs{0.0};

    // P13 per-stage timing rings (single decode-thread writer; GetDiagnostics
    // is the only reader and tolerates torn/interleaved samples).
    std::array<float, kTimingRingSize> m_decodeMsRing{};
    std::array<float, kTimingRingSize> m_vpMsRing{};
    size_t m_decodeMsCount = 0;   // decode-thread owned
    size_t m_vpMsCount     = 0;   // decode-thread owned
};

} // namespace rex::wallpaper

``

## src/wallpaper/decoder/WICTextureLoader.cpp

``cpp
#include "WICTextureLoader.hpp"

#include <shared/Logger.hpp>

#include <wincodec.h>
#include <wrl/client.h>

#include <codecvt>
#include <locale>

namespace rex::wallpaper {

namespace {

    // RAII COM guard — initializes COM on construction, uninitializes on destruction.
    // Skips uninit if COM was already initialized on this thread with a different mode.
    class ComGuard {
    public:
        ComGuard() {
            HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            m_initialized = SUCCEEDED(hr);
            if (!m_initialized && hr == RPC_E_CHANGED_MODE) {
                // COM already initialized with a different threading model; just use it
                m_initialized = false;
            }
        }
        ~ComGuard() {
            if (m_initialized) {
                CoUninitialize();
            }
        }
        ComGuard(const ComGuard&) = delete;
        ComGuard& operator=(const ComGuard&) = delete;

    private:
        bool m_initialized = false;
    };

    // Converts UTF-8 string_view to wide string for WIC APIs.
    std::wstring ToWide(std::string_view sv) {
        if (sv.empty()) return {};
        int len = MultiByteToWideChar(CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()), nullptr, 0);
        if (len <= 0) return {};
        std::wstring result(len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()), result.data(), len);
        return result;
    }

} // anonymous namespace

TextureData DecodeTextureFromFile(std::string_view path) {
    TextureData result;
    ComGuard com;

    // Create WIC imaging factory
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));

    if (FAILED(hr)) {
        REX_LOG_ERROR("WICLoader", "CoCreateInstance(IWICImagingFactory) failed: {:08x}", hr);
        return result;
    }

    // Create decoder
    std::wstring wpath = ToWide(path);
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromFilename(
        wpath.c_str(),
        nullptr,
        GENERIC_READ,
        WICDecodeMetadataCacheOnDemand,
        &decoder);

    if (FAILED(hr)) {
        REX_LOG_ERROR("WICLoader", "CreateDecoderFromFilename failed: {:08x} (path: {})",
                      hr, path);
        return result;
    }

    // Get first frame
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) {
        REX_LOG_ERROR("WICLoader", "GetFrame(0) failed: {:08x}", hr);
        return result;
    }

    // Convert to RGBA8
    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr)) {
        REX_LOG_ERROR("WICLoader", "CreateFormatConverter failed: {:08x}", hr);
        return result;
    }

    hr = converter->Initialize(
        frame.Get(),
        GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone,
        nullptr,
        0.0f,
        WICBitmapPaletteTypeCustom);

    if (FAILED(hr)) {
        REX_LOG_ERROR("WICLoader", "FormatConverter::Initialize failed: {:08x}", hr);
        return result;
    }

    // Get dimensions
    UINT width = 0, height = 0;
    converter->GetSize(&width, &height);
    result.width = static_cast<int>(width);
    result.height = static_cast<int>(height);
    result.rowPitch = result.width * 4; // RGBA8 = 4 bytes per pixel

    // Copy pixels
    UINT bufferSize = result.rowPitch * result.height;
    result.pixels.resize(bufferSize);

    hr = converter->CopyPixels(
        nullptr,
        static_cast<UINT>(result.rowPitch),
        bufferSize,
        result.pixels.data());

    if (FAILED(hr)) {
        REX_LOG_ERROR("WICLoader", "CopyPixels failed: {:08x}", hr);
        result.pixels.clear();
        result.width = 0;
        result.height = 0;
        result.rowPitch = 0;
        return result;
    }

    REX_LOG_INFO("WICLoader", "Decoded {}x{} image ({} bytes) from {}",
                 result.width, result.height, bufferSize, path);

    return result;
}

} // namespace rex::wallpaper

``

## src/wallpaper/decoder/WICTextureLoader.hpp

``cpp
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace rex::wallpaper {

struct TextureData {
    std::vector<uint8_t> pixels;
    int width = 0;
    int height = 0;
    int rowPitch = 0; // bytes per row (RGBA8 = width * 4)
};

/// Decodes an image file (JPG, PNG, BMP, TIFF) via Windows Imaging Component.
/// Returns RGBA8 pixel data, or an empty TextureData on failure.
/// The calling thread must have COM initialized (COINIT_APARTMENTTHREADED or
/// COINIT_MULTITHREADED).
TextureData DecodeTextureFromFile(std::string_view path);

} // namespace rex::wallpaper

``

## src/wallpaper/video/VideoManager.cpp

``cpp
#include "wallpaper/video/VideoManager.hpp"

#include <shared/Logger.hpp>

#include <cstdio>
#include <string>
#include <dxgi1_6.h>

namespace rex::wallpaper {

namespace {
constexpr const char* kModule = "VideoManager";
} // namespace

shared::Result<void> VideoManager::Initialize(const WallpaperConfig& config,
                                              ID3D12Device* d3d12Device) {
    if (m_initialized) {
        return shared::Result<void>();  // already initialized
    }
    if (!d3d12Device) {
        return shared::Result<void>(shared::ErrorCode::InvalidArgument,
                                    "a D3D12 device is required");
    }

    m_d3d12Device = d3d12Device;
    m_config = config;

    // D3D11 device on the SAME adapter as the D3D12 device: hardware video
    // decode + textures shareable with D3D12. The documented D3D12 way to
    // recover the adapter is the device LUID (IDXGIDevice QI is D3D11-only).
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->EnumAdapterByLuid(d3d12Device->GetAdapterLuid(),
                                          IID_PPV_ARGS(&adapter)))) {
        return shared::Result<void>(shared::ErrorCode::HardwareNotSupported,
                                    "cannot query D3D12 adapter");
    }
    const UINT createFlags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT |
                             D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                   createFlags, &fl, 1, D3D11_SDK_VERSION,
                                   &m_device11, nullptr, &m_context11);
    if (FAILED(hr)) {
        char buf[32] = {};
        std::snprintf(buf, sizeof(buf), "0x%08lx", static_cast<unsigned long>(hr));
        return shared::Result<void>(shared::ErrorCode::HardwareNotSupported,
                                    "D3D11 device creation failed (" + std::string(buf) + ")");
    }

    // Shared fence: D3D12 creates, D3D11 opens (orders blt before draw on
    // the same shared pool texture).
    hr = d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_SHARED,
                                  IID_PPV_ARGS(&m_fence12));
    if (SUCCEEDED(hr)) {
        hr = d3d12Device->CreateSharedHandle(m_fence12.Get(), nullptr,
                                             GENERIC_ALL, nullptr,
                                             &m_fenceHandle);
    }
    if (FAILED(hr)) {
        Shutdown();
        return shared::Result<void>(shared::ErrorCode::DeviceLost,
                                    "shared fence creation failed");
    }

    m_controller = std::make_unique<VideoPlaybackController>();
    m_initialized = true;
    REX_LOG_INFO(kModule, "initialized (D3D11 on D3D12 adapter, hw accel {})",
                 config.enableHardwareAcceleration ? "on" : "off");
    return shared::Result<void>();
}

shared::Result<void> VideoManager::Reinitialize(const WallpaperConfig& config,
                                                ID3D12Device* d3d12Device) {
    Shutdown();
    return Initialize(config, d3d12Device);
}

void VideoManager::Shutdown() {
    if (m_controller) {
        m_controller->Close();
        m_controller.reset();
    }
    if (m_fenceHandle) {
        CloseHandle(m_fenceHandle);
        m_fenceHandle = nullptr;
    }
    m_fence12.Reset();
    // Keep the device alive through the immediate context's teardown: the
    // context's destructor releases its internal device reference and then
    // continues using the device core. Without this extra ref, the core is
    // freed mid-teardown -> use-after-free inside d3d11.dll (intermittent
    // crash calling a freed block at dev-0x890).
    if (m_device11) {
        m_device11->AddRef();
    }
    m_device11.Reset();
    m_context11.Reset();
    m_device11.Reset();
    m_d3d12Device = nullptr;
    m_initialized = false;
}

shared::Result<void> VideoManager::OpenVideo(std::string_view path) {
    if (!m_controller) {
        return shared::Result<void>(shared::ErrorCode::NotInitialized,
                                    "video manager not initialized");
    }

    // Cache hit: the same file is already open — restart instead of reloading.
    if (m_controller->IsOpen() && m_currentPath == path) {
        m_controller->Seek(0.0);
        m_controller->Play();
        return shared::Result<void>();
    }

    // Trim the MRU bookkeeping to the max cache size.
    while (m_recentVideos.size() >= kMaxCacheEntries) {
        m_recentVideos.pop_front();
    }

    m_controller->Close();
    const bool hw = m_config.enableHardwareAcceleration && m_device11;
    shared::Result<void> result = m_controller->Open(path, hw,
                                                     m_context11.Get(), m_fenceHandle);
    if (result.IsError()) {
        m_currentPath.clear();
        return result;
    }
    m_surfaceGen.fetch_add(1, std::memory_order_relaxed);
    m_currentPath = std::string(path);
    m_recentVideos.push_back({m_currentPath, std::chrono::steady_clock::now()});

    // Auto-select the stream closest to the target monitor's resolution.
    {
        std::lock_guard<std::mutex> lock(m_monitorMutex);
        const MonitorInfo* target = nullptr;
        for (const auto& entry : m_monitorEntries) {
            if (!entry.info.connected) continue;
            target = &entry.info; // first connected monitor as fallback
            if (entry.info.isPrimary) break; // prefer the primary
        }
        if (target) {
            m_controller->SelectBestStream(target->width, target->height);
        }
    }
    return shared::Result<void>();
}

void VideoManager::Close() {
    if (m_controller) {
        m_controller->Close();
    }
    m_surfaceGen.fetch_add(1, std::memory_order_relaxed);
    m_currentPath.clear();
}

uint64_t VideoManager::GetAdapterLuid() const {
    if (!m_device11) {
        return 0;
    }
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(m_device11.As(&dxgiDevice))) {
        return 0;
    }
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(&adapter))) {
        return 0;
    }
    DXGI_ADAPTER_DESC desc{};
    if (FAILED(adapter->GetDesc(&desc))) {
        return 0;
    }
    const LUID luid = desc.AdapterLuid;
    return static_cast<uint64_t>(luid.LowPart) |
           (static_cast<uint64_t>(luid.HighPart) << 32);
}

uint64_t VideoManager::GetD3D12AdapterLuid() const {
    if (!m_d3d12Device) {
        return 0;
    }
    const LUID luid = m_d3d12Device->GetAdapterLuid();
    return static_cast<uint64_t>(luid.LowPart) |
           (static_cast<uint64_t>(luid.HighPart) << 32);
}

bool VideoManager::IsDecoderRendererSameAdapter() const {
    const uint64_t dec = GetAdapterLuid();
    const uint64_t rend = GetD3D12AdapterLuid();
    return dec != 0 && rend != 0 && dec == rend;
}

void VideoManager::SetTargetResolution(int targetWidth, int targetHeight) {
    if (!m_controller || !m_controller->IsOpen()) return;
    m_controller->SelectBestStream(targetWidth, targetHeight);
}

void VideoManager::Play() {
    if (m_controller) m_controller->Play();
}

void VideoManager::Pause() {
    if (m_controller) m_controller->Pause();
}

void VideoManager::Stop() {
    if (m_controller) m_controller->Stop();
}

void VideoManager::SetLoop(bool loop) {
    if (m_controller) m_controller->SetLoop(loop);
}

void VideoManager::SetSpeed(float speed) {
    if (m_controller) m_controller->SetSpeed(speed);
}

void VideoManager::SetVolume(float volume) {
    if (m_controller) m_controller->SetVolume(volume);
}

void VideoManager::SetMute(bool mute) {
    if (m_controller) m_controller->SetMute(mute);
}

shared::Result<void> VideoManager::Seek(double seconds) {
    if (!m_controller) {
        return shared::Result<void>(shared::ErrorCode::NotInitialized,
                                    "video manager not initialized");
    }
    return m_controller->Seek(seconds);
}

const VideoFrame* VideoManager::Tick(double targetPts) {
    return m_controller ? m_controller->Tick(targetPts) : nullptr;
}

// ─── Per-monitor compositing ─────────────────────────────────────────────

std::string VideoManager::AssignmentKey(const MonitorInfo& info) {
    // EDID hash is the stable identity across reconnects; fall back to the
    // device name when no EDID is available (e.g. remote sessions).
    if (info.edidHash != 0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%016llx",
                      static_cast<unsigned long long>(info.edidHash));
        return std::string("monitor.wallpaper.") + buf;
    }
    return "monitor.wallpaper." + info.id;
}

void VideoManager::SetSettingsService(
    std::shared_ptr<rex::settings::ISettingsService> settings) {
    {
        std::lock_guard<std::mutex> lock(m_monitorMutex);
        m_settings = std::move(settings);
    }
    // Assignments may have been persisted by a previous session (or the
    // module host); apply them now that the service is available.
    RestoreMonitorAssignments();
}

shared::Result<void> VideoManager::AddMonitor(const MonitorInfo& info) {
    if (!m_initialized || !m_d3d12Device) {
        return shared::Result<void>(shared::ErrorCode::NotInitialized,
                                    "video manager not initialized");
    }

    std::lock_guard<std::mutex> lock(m_monitorMutex);

    for (const auto& entry : m_monitorEntries) {
        if (entry.info.id == info.id) {
            return shared::Result<void>(); // already composited
        }
    }

    auto compositor = std::make_unique<MonitorCompositor>(info);
    if (m_workerW && IsWindow(m_workerW)) {
        if (!compositor->Initialize(m_d3d12Device, m_workerW)) {
            return shared::Result<void>(shared::ErrorCode::UnknownError,
                                        "monitor compositor init failed for " +
                                        info.id);
        }
    } else {
        REX_LOG_WARN("VideoManager",
                     "No wallpaper WorkerW; monitor '{}' compositor is dormant "
                     "(will activate when the host provides a layer)",
                     info.id.c_str());
    }

    m_monitorEntries.push_back({info, "", std::move(compositor)});
    REX_LOG_INFO("VideoManager", "monitor '{}' {}x{} @ ({},{}) added ({} active)",
                 info.id.c_str(), info.width, info.height, info.x, info.y,
                 m_monitorEntries.size());
    return shared::Result<void>();
}

shared::Result<void> VideoManager::RemoveMonitor(const std::string& monitorId) {
    std::lock_guard<std::mutex> lock(m_monitorMutex);

    for (auto it = m_monitorEntries.begin(); it != m_monitorEntries.end(); ++it) {
        if (it->info.id == monitorId) {
            if (it->compositor) {
                it->compositor->Shutdown();
            }
            m_monitorEntries.erase(it);
            REX_LOG_INFO("VideoManager", "monitor '{}' removed ({} active)",
                         monitorId.c_str(), m_monitorEntries.size());
            return shared::Result<void>();
        }
    }
    return shared::Result<void>(shared::ErrorCode::NotFound,
                                "monitor not found: " + monitorId);
}

void VideoManager::UpdateMonitor(const MonitorInfo& info) {
    std::lock_guard<std::mutex> lock(m_monitorMutex);

    for (auto& entry : m_monitorEntries) {
        if (entry.info.id != info.id) continue;

        entry.info = info;
        if (entry.compositor && entry.compositor->IsActive()) {
            entry.compositor->SetPosition(info.x, info.y);
            entry.compositor->Resize(info.width, info.height);
        }
        REX_LOG_DEBUG("VideoManager", "monitor '{}' updated to {}x{} @ ({},{})",
                      info.id.c_str(), info.width, info.height, info.x, info.y);
        return;
    }
}

void VideoManager::PresentToAllMonitors(const VideoFrame& frame) {
    if (!frame.valid) return;

    std::lock_guard<std::mutex> lock(m_monitorMutex);

    // P12 diagnostic: one decoder feeds every compositor the SAME frame.
    // Throttled (~1 line / 600 frames) so Trace logs stay readable.
    if ((frame.frameIndex % 600) == 1) {
        REX_LOG_DEBUG("VideoManager",
                      "presenting frame {} to {} monitor compositors "
                      "(shared single decode)",
                      frame.frameIndex, m_monitorEntries.size());
    }

    for (auto& entry : m_monitorEntries) {
        if (entry.compositor && entry.compositor->IsActive()) {
            entry.compositor->PresentFrame(frame, m_fence12.Get());
        }
    }
}

shared::Result<void> VideoManager::SetPerMonitorWallpaper(const std::string& monitorId,
                                                          const std::string& path) {
    std::lock_guard<std::mutex> lock(m_monitorMutex);

    for (auto& entry : m_monitorEntries) {
        if (entry.info.id != monitorId) continue;

        entry.assignedWallpaper = path;

        if (!entry.compositor || !entry.compositor->IsActive()) {
            return shared::Result<void>();
        }

        if (path.empty()) {
            // Clear the override: the engine re-pushes the main scene via
            // its LoadScene fan-out on the next scene refresh.
            entry.compositor->ClearStaticTexture();
            entry.compositor->ClearVideoFrame();
        } else {
            entry.compositor->PresentStaticTexture(path);
        }

        // Persist (best-effort; no-op without a settings service). The key
        // is dynamic (EDID hash) and unknown at module registration, so
        // register it on first use — addSetting is idempotent per key.
        if (m_settings) {
            rex::settings::Setting dynamicSetting;
            dynamicSetting.id = AssignmentKey(entry.info);
            dynamicSetting.name = "Monitor wallpaper " + entry.info.id;
            dynamicSetting.description =
                "Wallpaper assigned to monitor " + entry.info.id;
            dynamicSetting.type = rex::settings::SettingType::String;
            dynamicSetting.defaultValue = std::string("");
            m_settings->registerSetting(dynamicSetting);
            m_settings->setValue(AssignmentKey(entry.info),
                                 rex::settings::SettingValue(path));
        }
        REX_LOG_INFO("VideoManager", "monitor '{}' wallpaper: {}",
                     monitorId.c_str(),
                     path.empty() ? std::string("(main scene)")
                                  : std::string("\"") + path + "\"");
        return shared::Result<void>();
    }
    return shared::Result<void>(shared::ErrorCode::NotFound,
                                "monitor not found: " + monitorId);
}

void VideoManager::RestoreMonitorAssignments() {
    std::lock_guard<std::mutex> lock(m_monitorMutex);

    if (!m_settings) {
        REX_LOG_DEBUG("VideoManager",
                      "No settings service; skipping per-monitor restore");
        return;
    }

    for (auto& entry : m_monitorEntries) {
        auto value = m_settings->getValue(AssignmentKey(entry.info));
        if (!value || !std::holds_alternative<std::string>(*value)) {
            continue;
        }
        const std::string& path = std::get<std::string>(*value);
        if (path.empty()) continue;

        entry.assignedWallpaper = path;
        if (entry.compositor && entry.compositor->IsActive()) {
            entry.compositor->PresentStaticTexture(path);
            REX_LOG_INFO("VideoManager", "restored wallpaper for monitor '{}': {}",
                         entry.info.id.c_str(), path.c_str());
        }
    }
}

std::vector<MonitorInfo> VideoManager::GetActiveMonitors() const {
    std::lock_guard<std::mutex> lock(m_monitorMutex);

    std::vector<MonitorInfo> out;
    out.reserve(m_monitorEntries.size());
    for (const auto& entry : m_monitorEntries) {
        out.push_back(entry.info);
    }
    return out;
}

void VideoManager::ApplyFillMode(FillMode mode) {
    std::lock_guard<std::mutex> lock(m_monitorMutex);
    for (auto& entry : m_monitorEntries) {
        if (entry.compositor) {
            entry.compositor->SetFillMode(mode);
        }
    }
}

void VideoManager::ApplyTransform(float rotationDegrees, bool flipH,
                                  bool flipV) {
    std::lock_guard<std::mutex> lock(m_monitorMutex);
    for (auto& entry : m_monitorEntries) {
        if (entry.compositor) {
            entry.compositor->SetTransform(rotationDegrees, flipH, flipV);
        }
    }
}

void VideoManager::ApplyVideoAdjustments(float brightness, float contrast,
                                         float saturation) {
    std::lock_guard<std::mutex> lock(m_monitorMutex);
    for (auto& entry : m_monitorEntries) {
        if (entry.compositor) {
            entry.compositor->SetVideoAdjustments(brightness, contrast,
                                                  saturation);
        }
    }
}

void VideoManager::SetSharedStaticWallpaper(const std::string& path) {
    std::lock_guard<std::mutex> lock(m_monitorMutex);
    for (auto& entry : m_monitorEntries) {
        if (!entry.compositor || !entry.compositor->IsActive()) continue;
        if (!entry.assignedWallpaper.empty()) continue; // has an override
        entry.compositor->PresentStaticTexture(path);
    }
}

void VideoManager::ClearVideoFrames() {
    std::lock_guard<std::mutex> lock(m_monitorMutex);
    for (auto& entry : m_monitorEntries) {
        if (entry.compositor) {
            entry.compositor->ClearStaticTexture();
            entry.compositor->ClearVideoFrame();
        }
    }
}

void VideoManager::ShutdownMonitorCompositors() {
    std::lock_guard<std::mutex> lock(m_monitorMutex);
    for (auto& entry : m_monitorEntries) {
        if (entry.compositor) {
            entry.compositor->Shutdown();
        }
    }
    m_monitorEntries.clear();
}

} // namespace rex::wallpaper

``

## src/wallpaper/video/VideoManager.hpp

``cpp
#pragma once

#include "wallpaper/playback/VideoPlaybackController.hpp"
#include "wallpaper/monitor/MonitorCompositor.hpp"
#include "modules/Settings/interfaces/ISettingsService.hpp"

#include <rex/wallpaper/IWallpaperEngine.hpp>
#include <shared/Result.hpp>

#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace rex::wallpaper {

// ─── VideoManager ───────────────────────────────────────────────────────
//
// Persistent runtime facade for FFmpeg GPU video playback (Phase 2 backend).
// Owns the same-adapter D3D11 device and the shared D3D11/D3D12 fence; every
// published frame lives in a D3D11 pool slot that the D3D12 renderer opens
// through NT shared handles.
//
// D3D12 renderer contract:
//   1. Open each slot once: d3d12Device->OpenSharedHandle(GetSlotHandle(slot))
//   2. Before sampling slot N: d3d12Queue->Wait(GetFence(), GetSlotFence(N))
//      (monotonic values: always race-free, even across ring wrap-around)
//
// Threading: control methods (Open/Play/Pause/Stop/Seek) are safe from any
// thread; Tick() is the render-thread-only consumer (SPSC).

class VideoManager {
public:
    VideoManager() = default;
    ~VideoManager() { Shutdown(); }

    VideoManager(const VideoManager&) = delete;
    VideoManager& operator=(const VideoManager&) = delete;

    /// d3d12Device is borrowed (must outlive this manager). The D3D11
    /// device is created on the exact same adapter so decode textures and
    /// shared handles interoperate with D3D12.
    shared::Result<void> Initialize(const WallpaperConfig& config,
                                    ID3D12Device* d3d12Device);
    /// Tear down everything (video, D3D11 device, shared fence) and
    /// reinitialize on a (possibly new) D3D12 device. Used by device-loss
    /// recovery: the old D3D11/D3D12 resources are dead after the loss.
    shared::Result<void> Reinitialize(const WallpaperConfig& config,
                                      ID3D12Device* d3d12Device);
    void Shutdown();
    bool IsInitialized() const { return m_initialized; }

    shared::Result<void> OpenVideo(std::string_view path);
    void Close();

    /// Adaptive resolution: picks the video stream that best fits the
    /// target monitor resolution (see FfmpegVideoDecoder::SelectStreamByResolution).
    void SetTargetResolution(int targetWidth, int targetHeight);

    void Play();
    void Pause();
    void Stop();
    void SetLoop(bool loop);
    void SetSpeed(float speed);
    void SetVolume(float volume);
    void SetMute(bool mute);
    shared::Result<void> Seek(double seconds);

    /// Render-thread consumer: latest GPU frame or nullptr.
    const VideoFrame* Tick(double targetPts);

    bool IsOpen()    const { return m_controller && m_controller->IsOpen(); }
    bool IsPlaying() const { return m_controller && m_controller->IsPlaying(); }

    double GetPosition() const { return m_controller ? m_controller->GetPosition() : 0.0; }
    double GetDuration()  const { return m_controller ? m_controller->GetDuration() : 0.0; }
    double GetFrameRate() const { return m_controller ? m_controller->GetFrameRate() : 0.0; }
    int    GetWidth()     const { return m_controller ? m_controller->GetWidth() : 0; }
    int    GetHeight()    const { return m_controller ? m_controller->GetHeight() : 0; }

    // ─── Shared-surface contract (D3D12 renderer) ──────────────────────
    // The D3D12 command queue must Wait on this fence with the frame's
    // fenceValue before sampling a pool slot.
    ID3D12Fence* GetSharedFence() const { return m_fence12.Get(); }
    HANDLE GetSlotHandle(UINT slot) const {
        return m_controller ? m_controller->GetSlotHandle(slot) : nullptr;
    }
    UINT64 GetSlotFence(UINT slot) const {
        return m_controller ? m_controller->GetSlotFence(slot) : 0;
    }

    // ─── Frame surface (cross-process preview) ─────────────────────────
    /// NT handle of the shared D3D11/D3D12 fence (process-local).
    HANDLE GetFenceHandle() const { return m_fenceHandle; }
    /// Adapter LUID as a single 64-bit value (LowPart | HighPart<<32).
    uint64_t GetAdapterLuid() const;
    /// LUID of the adapter the D3D12 renderer runs on (0 = unknown).
    uint64_t GetD3D12AdapterLuid() const;
    /// True when the D3D11 decoder device and the D3D12 renderer device
    /// live on the same physical adapter (required for shared handles).
    bool IsDecoderRendererSameAdapter() const;
    /// Bumped on every OpenVideo/Close; hosts re-duplicate handles on change.
    uint64_t GetSurfaceGeneration() const { return m_surfaceGen; }
    UINT GetSlotCount() const {
        return m_controller ? m_controller->GetSlotCount() : 0;
    }
    VideoPlaybackController::FrameSnapshot GetLastFrame() const {
        return m_controller ? m_controller->GetLastFrame() : VideoPlaybackController::FrameSnapshot{};
    }

    VideoPlaybackTelemetry GetTelemetry() const {
        return m_controller ? m_controller->GetTelemetry() : VideoPlaybackTelemetry{};
    }

    // ─── Per-monitor compositing (Phase 5) ─────────────────────────────
    /// Wallpaper layer window: parent for per-monitor compositor windows.
    /// Only meaningful for the MultiMonitor desktop host.
    void SetWallpaperHostWindow(HWND workerW) { m_workerW = workerW; }

    /// Settings service for per-monitor wallpaper persistence (optional;
    /// persistence is a no-op until this is set). Setting it triggers a
    /// restore of persisted assignments.
    void SetSettingsService(std::shared_ptr<rex::settings::ISettingsService> settings);

    /// Create a compositor for a monitor. Safe to call after Initialize()
    /// (renders on the render thread; control methods are mutex-guarded).
    shared::Result<void> AddMonitor(const MonitorInfo& info);
    shared::Result<void> RemoveMonitor(const std::string& monitorId);
    /// Refresh geometry (resolution/orientation changes). Recreates the
    /// compositor when the renderer cannot resize.
    void UpdateMonitor(const MonitorInfo& info);

    /// Render-thread: present the just-ticked frame to every monitor
    /// compositor (each does its own Begin/Draw/Render/End).
    void PresentToAllMonitors(const VideoFrame& frame);

    /// Per-monitor wallpaper override. Empty path clears the override
    /// (the monitor falls back to the main scene, which the engine
    /// re-pushes via its LoadScene fan-out).
    shared::Result<void> SetPerMonitorWallpaper(const std::string& monitorId,
                                                const std::string& path);

    /// Load persisted per-monitor assignments (EDID-keyed) and apply them.
    void RestoreMonitorAssignments();

    /// Snapshot of the monitors currently composited (render-thread data).
    std::vector<MonitorInfo> GetActiveMonitors() const;

    /// Fan-out of the main renderer's fill mode.
    void ApplyFillMode(FillMode mode);

    /// Fan-out of the main renderer's transform (rotation/flip).
    void ApplyTransform(float rotationDegrees, bool flipH, bool flipV);

    /// Fan-out of the main renderer's video adjustments.
    void ApplyVideoAdjustments(float brightness, float contrast,
                               float saturation);

    /// Fan-out of the main scene's static texture to monitors WITHOUT a
    /// per-monitor override (they mirror the main scene).
    void SetSharedStaticWallpaper(const std::string& path);
    /// Drop GPU video/static state on all compositors (scene unload).
    void ClearVideoFrames();

    /// Shutdown all monitor compositors (call on shutdown/device loss).
    void ShutdownMonitorCompositors();

private:
    ID3D12Device* m_d3d12Device = nullptr;  // borrowed
    WallpaperConfig m_config{};

    Microsoft::WRL::ComPtr<ID3D11Device>        m_device11;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context11;
    Microsoft::WRL::ComPtr<ID3D12Fence>         m_fence12;
    HANDLE m_fenceHandle = nullptr;

    std::unique_ptr<VideoPlaybackController> m_controller;
    bool m_initialized = false;
    std::atomic<uint64_t> m_surfaceGen{0};

    // ─── Shared wallpaper cache ──────────────────────────────────────────
    // MRU bookkeeping of recently used wallpapers (max kMaxCacheEntries).
    // The current controller is reused when the same file is re-requested.
    struct CacheEntry {
        std::string path;
        std::chrono::steady_clock::time_point lastUsed;
    };
    static constexpr size_t kMaxCacheEntries = 3;
    std::deque<CacheEntry> m_recentVideos;
    std::string m_currentPath;

    // ─── Per-monitor compositing ─────────────────────────────────────────
    // Entries are guarded so IPC threads can Add/Remove monitors while the
    // render thread presents (each entry's compositor is owned here).
    struct MonitorEntry {
        MonitorInfo info;
        std::string assignedWallpaper; // per-monitor override ("" = main scene)
        std::unique_ptr<MonitorCompositor> compositor;
    };
    std::vector<MonitorEntry> m_monitorEntries;
    mutable std::mutex m_monitorMutex;
    HWND m_workerW = nullptr;
    std::shared_ptr<rex::settings::ISettingsService> m_settings;

    static std::string AssignmentKey(const MonitorInfo& info);
};

} // namespace rex::wallpaper

``

## src/wallpaper/playback/VideoPlaybackController.cpp

``cpp
#include "wallpaper/playback/VideoPlaybackController.hpp"

#include <shared/Logger.hpp>

#include <chrono>

namespace rex::wallpaper {

namespace {
constexpr const char* kModule = "VideoPlaybackController";
using ms = std::chrono::duration<double, std::milli>;
} // namespace

VideoPlaybackController::VideoPlaybackController() {
    m_fpsStartTime = Clock::now();
}

VideoPlaybackController::~VideoPlaybackController() {
    Close();
}

shared::Result<void> VideoPlaybackController::Open(std::string_view path,
                                                   bool useHardware,
                                                   ID3D11DeviceContext* context11,
                                                   HANDLE fenceHandle) {
    std::lock_guard lock(m_threadMutex);
    if (m_open.load(std::memory_order_acquire)) {
        return shared::Result<void>(shared::ErrorCode::AlreadyInitialized,
                                    "controller already open");
    }

    auto decoder = std::make_unique<FfmpegVideoDecoder>();
    shared::Result<void> result = decoder->Open(path, useHardware, context11, fenceHandle);
    if (result.IsError()) {
        return result;
    }

    m_decoder = std::move(decoder);
    m_fenceHandle = fenceHandle;
    m_duration  = m_decoder->GetDuration();
    m_frameRate = m_decoder->GetFrameRate();
    m_frameIntervalMs = m_frameRate > 0.0 ? 1000.0 / m_frameRate : 33.333;
    m_width  = m_decoder->GetWidth();
    m_height = m_decoder->GetHeight();

    // Audio is optional and never fails the video: no audio track or a
    // broken audio device just means a silent wallpaper.
    m_audio = std::make_unique<AudioPlayer>();
    shared::Result<void> audioResult = m_audio->Open(path);
    if (audioResult.IsError()) {
        REX_LOG_WARN("VideoPlaybackController",
                     "audio disabled: {}", audioResult.ToString());
        m_audio.reset();
    }

    m_position.store(0.0, std::memory_order_release);
    m_playing.store(false, std::memory_order_release);
    m_paused.store(false, std::memory_order_release);
    m_open.store(true, std::memory_order_release);

    m_framesDecoded.store(0, std::memory_order_relaxed);
    m_framesConsumed.store(0, std::memory_order_relaxed);
    m_framesDropped.store(0, std::memory_order_relaxed);

    // ── Prewarm (items 5+6): decode + publish the first frame HERE ─────
    // Two wins: the surface pool is built synchronously inside Open()
    // (resolution is known from the real frame — no lazy mid-playback
    // rebuild), and Play() can present instantly without waiting a full
    // decoder interval for frame #1.
    if (m_prewarmEnabled) {
        VideoFrame warm;
        {
            std::lock_guard lock(m_decodeMutex);
            if (m_decoder->DecodeFrame(warm)) {
                m_scheduler.Push(warm);
                m_decoderFrameCount = 1;
                m_framesDecoded.store(1, std::memory_order_relaxed);
                m_position.store(warm.pts, std::memory_order_release);
                std::lock_guard lastLock(m_lastFrameMutex);
                m_lastFrame.slot       = warm.slot;
                m_lastFrame.fenceValue = warm.fenceValue;
                m_lastFrame.width      = warm.width;
                m_lastFrame.height     = warm.height;
                m_lastFrame.frameIndex = warm.frameIndex;
            } else {
                REX_LOG_WARN("VideoPlaybackController",
                             "prewarm decode failed; first frame will arrive "
                             "from the decode thread");
            }
        }
    }

    m_consumerFrameCount = 0;
    m_fpsStartTime = Clock::now();

    m_threadRunning.store(true, std::memory_order_release);
    m_thread = std::thread(&VideoPlaybackController::DecoderThread, this);
    return shared::Result<void>();
}

void VideoPlaybackController::Close() {
    std::lock_guard lock(m_threadMutex);
    m_threadRunning.store(false, std::memory_order_release);
    m_playing.store(false, std::memory_order_release);
    if (m_thread.joinable()) {
        m_thread.join();
    }
    {
        std::lock_guard lastLock(m_lastFrameMutex);
        m_lastFrame = FrameSnapshot{};
    }
    m_fenceHandle = nullptr;
    if (m_audio) {
        m_audio->Close();
        m_audio.reset();
    }
    if (m_decoder) {
        m_decoder->Close();
    }
    m_decoder.reset();
    m_open.store(false, std::memory_order_release);
}

void VideoPlaybackController::Play() {
    if (!m_open.load(std::memory_order_acquire)) return;
    if (!m_playing.load(std::memory_order_acquire) &&
        m_position.load(std::memory_order_acquire) >= m_duration - 0.001) {
        Seek(0.0);
    }
    m_paused.store(false, std::memory_order_release);
    m_playing.store(true, std::memory_order_release);
    if (m_audio) {
        m_audio->Play();
    }
}

void VideoPlaybackController::Pause() {
    m_paused.store(true, std::memory_order_release);
    m_playing.store(false, std::memory_order_release);
    if (m_audio) {
        m_audio->Pause();
    }
}

void VideoPlaybackController::Stop() {
    m_paused.store(false, std::memory_order_release);
    m_playing.store(false, std::memory_order_release);
    m_position.store(0.0, std::memory_order_release);
    if (m_audio) {
        m_audio->Stop();
    }
}

void VideoPlaybackController::SetLoop(bool loop) {
    m_looping.store(loop, std::memory_order_relaxed);
    if (m_decoder) {
        m_decoder->SetLoop(loop);
    }
    if (m_audio) {
        m_audio->SetLoop(loop);
    }
}

void VideoPlaybackController::SetSpeed(float speed) {
    m_speed.store(speed > 0.0f ? speed : 1.0f, std::memory_order_relaxed);
}

void VideoPlaybackController::SetVolume(float volume) {
    if (m_audio) {
        m_audio->SetVolume(volume);
    }
}

void VideoPlaybackController::SetMute(bool mute) {
    if (m_audio) {
        m_audio->SetMute(mute);
    }
}

shared::Result<void> VideoPlaybackController::Seek(double seconds) {
    if (!m_open.load(std::memory_order_acquire) || !m_decoder) {
        return shared::Result<void>(shared::ErrorCode::NotInitialized,
                                    "controller not open");
    }
    std::lock_guard lock(m_decodeMutex);
    if (!m_decoder->Seek(seconds)) {
        return shared::Result<void>(shared::ErrorCode::InvalidState, "seek failed");
    }
    m_position.store(seconds, std::memory_order_release);
    if (m_audio) {
        m_audio->Seek(seconds);
    }
    return shared::Result<void>();
}

void VideoPlaybackController::SelectBestStream(int targetWidth, int targetHeight) {
    if (!m_open.load(std::memory_order_acquire) || !m_decoder) return;
    std::lock_guard lock(m_decodeMutex);  // excludes DecodeFrame and Seek
    m_decoder->SelectStreamByResolution(targetWidth, targetHeight);

    // The new stream may have different properties: refresh everything the
    // controller cached from the decoder (pacing, telemetry dimensions).
    m_duration  = m_decoder->GetDuration();
    m_frameRate = m_decoder->GetFrameRate();
    m_frameIntervalMs = m_frameRate > 0.0 ? 1000.0 / m_frameRate : 33.333;
    m_width  = m_decoder->GetWidth();
    m_height = m_decoder->GetHeight();
}

const VideoFrame* VideoPlaybackController::Tick(double targetPts) {
    const VideoFrame* frame = m_scheduler.Tick(targetPts);
    if (frame) {
        m_consumerFrameCount++;
        m_framesConsumed.fetch_add(1, std::memory_order_relaxed);
        const uint64_t decoded = m_framesDecoded.load(std::memory_order_relaxed);
        const uint64_t consumed = m_framesConsumed.load(std::memory_order_relaxed);
        m_framesDropped.store(decoded > consumed ? decoded - consumed - 1 : 0,
                              std::memory_order_relaxed);
    }
    return frame;
}

HANDLE VideoPlaybackController::GetSlotHandle(UINT slot) const {
    return m_decoder ? m_decoder->GetSlotHandle(slot) : nullptr;
}

UINT64 VideoPlaybackController::GetSlotFence(UINT slot) const {
    return m_decoder ? m_decoder->GetSlotFence(slot) : 0;
}

VideoPlaybackController::FrameSnapshot VideoPlaybackController::GetLastFrame() const {
    std::lock_guard lock(m_lastFrameMutex);
    return m_lastFrame;
}

VideoPlaybackTelemetry VideoPlaybackController::GetTelemetry() const {
    VideoPlaybackTelemetry t;
    t.framesDecoded = m_framesDecoded.load(std::memory_order_relaxed);
    t.framesConsumed = m_framesConsumed.load(std::memory_order_relaxed);
    t.framesDropped = m_framesDropped.load(std::memory_order_relaxed);
    const double secs = std::chrono::duration<double>(Clock::now() - m_fpsStartTime).count();
    t.decoderFps = secs > 0.0 ? static_cast<double>(m_decoderFrameCount) / secs : 0.0;
    t.renderFps = secs > 0.0 ? static_cast<double>(m_consumerFrameCount) / secs : 0.0;
    t.ringSlotCount = m_scheduler.GetNumSlots();
    t.ringBytes = static_cast<uint64_t>(m_width) * static_cast<uint64_t>(m_height) *
                  4 * m_scheduler.GetNumSlots();
    if (m_decoder) {
        const DecoderDiagnostics d = m_decoder->GetDiagnostics();
        t.hardwareAccelerated = d.hardwareAccelerated;
        t.gpuToCpuCopies = d.gpuToCpuCopies;
        t.cpuToGpuCopies = d.cpuToGpuCopies;
        t.uploadLatencyMs = d.lastDecodeMs;
        t.decodeMsP50 = d.decodeMsP50;
        t.decodeMsP95 = d.decodeMsP95;
        t.decodeMsP99 = d.decodeMsP99;
        t.vpMsP50     = d.vpMsP50;
        t.vpMsP95     = d.vpMsP95;
        t.vpMsP99     = d.vpMsP99;
        t.recoveryCount = d.recoveryCount;
        t.deviceResets = d.deviceResets;
    }
    return t;
}

void VideoPlaybackController::DecoderThread() {
    while (m_threadRunning.load(std::memory_order_acquire)) {
        if (!m_playing.load(std::memory_order_acquire) ||
            m_paused.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        const auto t0 = Clock::now();
        VideoFrame frame;
        bool ok = false;
        {
            std::lock_guard lock(m_decodeMutex);
            ok = m_decoder && m_decoder->DecodeFrame(frame);
        }
        if (!ok) {
            if (m_decoder && m_decoder->GetLoop()) {
                continue;  // decoder already looped internally
            }
            m_playing.store(false, std::memory_order_release);  // end of stream
            continue;
        }

        m_scheduler.Push(frame);
        m_decoderFrameCount++;
        m_framesDecoded.fetch_add(1, std::memory_order_relaxed);
        m_position.store(frame.pts, std::memory_order_release);

        {
            std::lock_guard lastLock(m_lastFrameMutex);
            m_lastFrame.slot = frame.slot;
            m_lastFrame.fenceValue = frame.fenceValue;
            m_lastFrame.width = frame.width;
            m_lastFrame.height = frame.height;
            m_lastFrame.frameIndex = frame.frameIndex;
        }

        // Pace decode to the source frame rate (speed-scaled).
        const double elapsed = ms(Clock::now() - t0).count();
        const double interval =
            m_frameIntervalMs / m_speed.load(std::memory_order_relaxed);
        const double remaining = interval - elapsed;
        if (remaining > 0.5) {
            std::this_thread::sleep_for(std::chrono::milliseconds(
                static_cast<long long>(remaining)));
        }
    }
}

} // namespace rex::wallpaper

``

## src/wallpaper/playback/VideoPlaybackController.hpp

``cpp
#pragma once

#include "wallpaper/decoder/FfmpegVideoDecoder.hpp"
#include "wallpaper/playback/FrameScheduler.hpp"
#include "wallpaper/audio/AudioPlayer.hpp"

#include <shared/Result.hpp>

#include <windows.h>
#include <d3d11_4.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>

namespace rex::wallpaper {

// ─── VideoPlaybackTelemetry ─────────────────────────────────────────────
//
// NOTE: intentionally distinct from PlaybackTelemetry (PlaybackController)
// to avoid a name collision while both controllers coexist in the binary.

struct VideoPlaybackTelemetry {
    uint64_t framesDecoded   = 0;
    uint64_t framesConsumed  = 0;
    uint64_t framesDropped   = 0;
    uint64_t gpuToCpuCopies  = 0;
    uint64_t cpuToGpuCopies  = 0;
    double   decoderFps      = 0.0;
    double   renderFps       = 0.0;
    double   uploadLatencyMs = 0.0;
    // P13 per-stage percentiles (see DecoderDiagnostics).
    double   decodeMsP50 = 0.0, decodeMsP95 = 0.0, decodeMsP99 = 0.0;
    double   vpMsP50     = 0.0, vpMsP95     = 0.0, vpMsP99     = 0.0;
    uint32_t recoveryCount   = 0;
    uint32_t deviceResets    = 0;
    uint64_t ringSlotCount   = 0;
    uint64_t ringBytes       = 0;
    bool     hardwareAccelerated = false;
};

// ─── VideoPlaybackController ────────────────────────────────────────────
//
// FFmpeg-based playback controller. Decodes on a background thread paced
// to the source frame rate and publishes VideoFrames through the SPSC
// FrameScheduler. Frames are GPU-resident (D3D11 pool + shared handles),
// so the render thread never touches CPU buffers.
//
// Threading:
//   - Decode thread (internal): producer of FrameScheduler.
//   - Render thread (external): Tick() consumer of FrameScheduler.
//   - Control (Play/Pause/Stop/Seek/SetLoop): atomic or mutex-guarded.

class VideoPlaybackController {
public:
    VideoPlaybackController();
    ~VideoPlaybackController();

    VideoPlaybackController(const VideoPlaybackController&) = delete;
    VideoPlaybackController& operator=(const VideoPlaybackController&) = delete;

    // context11: immediate context of the same-adapter D3D11 device
    // created by VideoManager. fenceHandle: NT handle of the shared
    // D3D11/D3D12 fence.
    shared::Result<void> Open(std::string_view path, bool useHardware,
                              ID3D11DeviceContext* context11, HANDLE fenceHandle);
    void Close();

    void Play();
    void Pause();
    void Stop();
    void SetLoop(bool loop);
    void SetSpeed(float speed);
    void SetVolume(float volume);
    void SetMute(bool mute);
    shared::Result<void> Seek(double seconds);

    /// Re-selects the decoder's video stream to best match the target
    /// resolution (adaptive resolution). Refreshes cached stream properties.
    void SelectBestStream(int targetWidth, int targetHeight);

    /// Returns the latest GPU frame since the last call, or nullptr.
    /// Caller must consume synchronously (wait on the slot fence before
    /// sampling) before the next Tick().
    const VideoFrame* Tick(double targetPts);

    bool IsOpen()    const { return m_open.load(std::memory_order_acquire); }
    bool IsPlaying() const { return m_playing.load(std::memory_order_acquire); }
    bool IsPaused()  const { return m_paused.load(std::memory_order_acquire); }

    double GetPosition() const { return m_position.load(std::memory_order_acquire); }
    double GetDuration()  const { return m_duration; }
    double GetFrameRate() const { return m_frameRate; }
    int    GetWidth()     const { return m_width; }
    int    GetHeight()    const { return m_height; }

    // Forwarded from the decoder (shared-surface contract for D3D12).
    HANDLE GetSlotHandle(UINT slot) const;
    UINT64 GetSlotFence(UINT slot) const;

    // ─── Frame surface (cross-process preview) ───────────────────────────
    struct FrameSnapshot {
        UINT slot = 0;
        UINT64 fenceValue = 0;
        UINT width = 0;
        UINT height = 0;
        uint64_t frameIndex = 0;
    };
    UINT GetSlotCount() const { return static_cast<UINT>(m_scheduler.GetNumSlots()); }
    /// Latest published frame identity; safe from any thread.
    FrameSnapshot GetLastFrame() const;

    VideoPlaybackTelemetry GetTelemetry() const;

private:
    void DecoderThread();

    std::unique_ptr<FfmpegVideoDecoder> m_decoder;
    std::unique_ptr<AudioPlayer> m_audio;
    FrameScheduler m_scheduler;

    std::mutex m_decodeMutex;  // serializes DecodeFrame vs Seek
    // P13 item 6: decode + publish frame #1 synchronously inside Open()
    // (also builds the surface pool before the decode thread starts).
    bool m_prewarmEnabled = true;

    // ─── Shared state ───────────────────────────────────────────────────
    std::atomic<bool> m_open{false};
    std::atomic<bool> m_playing{false};
    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_looping{true};
    std::atomic<float> m_speed{1.0f};
    std::atomic<double> m_position{0.0};

    double m_duration  = 0.0;
    double m_frameRate = 30.0;
    double m_frameIntervalMs = 33.333;
    int    m_width  = 0;
    int    m_height = 0;

    // ─── Telemetry ──────────────────────────────────────────────────────
    std::atomic<uint64_t> m_framesDecoded{0};
    std::atomic<uint64_t> m_framesConsumed{0};
    std::atomic<uint64_t> m_framesDropped{0};
    using Clock = std::chrono::steady_clock;
    Clock::time_point m_fpsStartTime;
    uint64_t m_decoderFrameCount  = 0;
    uint64_t m_consumerFrameCount = 0;

    // ─── Thread management ──────────────────────────────────────────────
    std::atomic<bool> m_threadRunning{false};
    std::thread m_thread;
    std::mutex m_threadMutex;

    // ─── Frame surface state ────────────────────────────────────────────
    HANDLE m_fenceHandle = nullptr;   // NT handle of the shared D3D12 fence
    mutable std::mutex m_lastFrameMutex;
    FrameSnapshot m_lastFrame;
};

} // namespace rex::wallpaper

``

## src/wallpaper/playback/FrameScheduler.cpp

``cpp
#include "wallpaper/playback/FrameScheduler.hpp"

namespace rex::wallpaper {

void FrameScheduler::Push(const VideoFrame& frame) {
    const uint64_t wc = m_writeCount.load(std::memory_order_relaxed);
    m_slots[wc % kNumSlots] = frame;
    m_writeCount.store(wc + 1, std::memory_order_release);
}

const VideoFrame* FrameScheduler::Tick(double /*targetPts*/) {
    const uint64_t wc = m_writeCount.load(std::memory_order_acquire);
    if (wc == 0 || wc - 1 <= m_lastConsumedWrite) {
        return nullptr;
    }
    m_lastConsumedWrite = wc - 1;
    return &m_slots[(wc - 1) % kNumSlots];
}

} // namespace rex::wallpaper

``

## src/wallpaper/playback/FrameScheduler.hpp

``cpp
#pragma once

#include "wallpaper/decoder/FfmpegVideoDecoder.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace rex::wallpaper {

// ─── FrameScheduler ─────────────────────────────────────────────────────
//
// SPSC ring of GPU-backed VideoFrames. Exactly one producer (decode thread)
// calls Push(); exactly one consumer (render thread) calls Tick().
//
// ─── Semantics ──────────────────────────────────────────────────────────
//
// Latest-frame-wins: the consumer always receives the most recently pushed
// frame; intermediate frames are counted as dropped. This is the correct
// policy for wallpaper playback (never queue more than one frame of latency).
//
// Frame slots are a stable ring array and the underlying pool textures are
// owned by the decoder for the process lifetime, so the returned pointer
// stays valid and no handles are ever closed here. GPU content safety is
// guaranteed by the D3D11->D3D12 fence: the consumer must wait on
// GetSlotFence(frame.slot) before sampling (see FfmpegVideoDecoder).
//
// ─── SPSC guarantee ─────────────────────────────────────────────────────
//
// Producer writes slot (wc % N), consumer reads slot ((wc - 1) % N); with
// the single-producer/single-consumer invariant these are always distinct
// indices, so no mutex is needed — only acquire/release atomics on the
// write count. If multiple producers or consumers are ever added, this
// class MUST be redesigned.

class FrameScheduler {
public:
    static constexpr size_t kNumSlots = 8;

    /// Producer: publish a frame. Overwrites the oldest unpublished slot;
    /// that slot is only read by the consumer after it has already moved
    /// past it, so no torn reads are possible.
    void Push(const VideoFrame& frame);

    /// Consumer: returns the latest published frame since the last call,
    /// or nullptr if nothing new arrived. Never blocks.
    const VideoFrame* Tick(double targetPts);

    size_t GetNumSlots() const { return kNumSlots; }

private:
    std::array<VideoFrame, kNumSlots> m_slots{};
    std::atomic<uint64_t> m_writeCount{0};
    uint64_t m_lastConsumedWrite = 0;  // consumer-local
};

} // namespace rex::wallpaper

``

## src/wallpaper/renderer/RenderPipeline.cpp

``cpp
#include "RenderPipeline.hpp"
#include <cstdio>
#include <cstring>
#include <d3dcompiler.h>

namespace rex::wallpaper {

bool RenderPipeline::Initialize(ID3D12Device* device, int width, int height) {
    m_device = device;
    m_width = width;
    m_height = height;

    // Root constants: b0 = view-proj (16 floats), b1 = world (16 floats)
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.RegisterSpace = 0;
    params[0].Constants.Num32BitValues = 16;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 1;
    params[1].Constants.RegisterSpace = 0;
    params[1].Constants.Num32BitValues = 16;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = 2;
    rsDesc.pParameters = params;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    Microsoft::WRL::ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
        &sig, &err))) {
        printf("[Pipeline] root sig serialize failed\n");
        return false;
    }
    if (FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(),
        sig->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature)))) {
        return false;
    }

    // Shaders
    const char* vsSrc =
        "struct VSInput { float3 pos : POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD; };"
        "struct VSOutput { float4 pos : SV_Position; float2 uv : TEXCOORD; };"
        "cbuffer VP : register(b0) { float4x4 viewProj; };"
        "cbuffer World : register(b1) { float4x4 world; };"
        "VSOutput main(VSInput input) {"
        "    VSOutput o;"
        "    o.pos = mul(mul(float4(input.pos, 1), world), viewProj);"
        "    o.uv = input.uv;"
        "    return o;"
        "}";

    const char* psSrc =
        "struct PSInput { float4 pos : SV_Position; float2 uv : TEXCOORD; };"
        "float4 main(PSInput input) : SV_Target {"
        "    return float4(input.uv, 0.5 + 0.5*sin(input.uv.x*10), 1);"
        "}";

    Microsoft::WRL::ComPtr<ID3DBlob> vs, ps;
    if (FAILED(D3DCompile(vsSrc, strlen(vsSrc), nullptr, nullptr, nullptr,
        "main", "vs_5_0", 0, 0, &vs, &err))) {
        printf("[Pipeline] VS compile failed\n");
        return false;
    }
    if (FAILED(D3DCompile(psSrc, strlen(psSrc), nullptr, nullptr, nullptr,
        "main", "ps_5_0", 0, 0, &ps, &err))) {
        printf("[Pipeline] PS compile failed\n");
        return false;
    }

    // Input layout
    D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = m_rootSignature.Get();
    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pso.InputLayout = { layout, 3 };
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.BlendState.RenderTarget[0].BlendEnable = FALSE;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.SampleDesc.Count = 1;

    if (FAILED(device->CreateGraphicsPipelineState(&pso,
        IID_PPV_ARGS(&m_pipelineState)))) {
        printf("[Pipeline] PSO creation failed\n");
        return false;
    }

    // Depth buffer
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC dd = {};
    dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    dd.Width = static_cast<UINT64>(width);
    dd.Height = static_cast<UINT>(height);
    dd.DepthOrArraySize = 1;
    dd.MipLevels = 1;
    dd.Format = DXGI_FORMAT_D32_FLOAT;
    dd.SampleDesc.Count = 1;
    dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE dc = {};
    dc.Format = DXGI_FORMAT_D32_FLOAT;
    dc.DepthStencil.Depth = 1.0f;
    dc.DepthStencil.Stencil = 0;

    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE,
        &dd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &dc,
        IID_PPV_ARGS(&m_depthTexture)))) {
        printf("[Pipeline] depth buffer creation failed\n");
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC dsvDesc = {};
    dsvDesc.NumDescriptors = 1;
    dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    if (FAILED(device->CreateDescriptorHeap(&dsvDesc,
        IID_PPV_ARGS(&m_dsvHeap)))) {
        return false;
    }

    D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
    dsv.Format = DXGI_FORMAT_D32_FLOAT;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsv.Flags = D3D12_DSV_FLAG_NONE;
    device->CreateDepthStencilView(m_depthTexture.Get(), &dsv,
        m_dsvHeap->GetCPUDescriptorHandleForHeapStart());

    printf("[Pipeline] initialized (%dx%d)\n", width, height);
    return true;
}

void RenderPipeline::Render(ID3D12GraphicsCommandList* cmdList,
                            const std::vector<Entity>& entities,
                            const XMMATRIX& viewProj,
                            const float clearColor[4]) {
    cmdList->SetGraphicsRootSignature(m_rootSignature.Get());
    cmdList->SetPipelineState(m_pipelineState.Get());

    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D12_VIEWPORT vp = { 0, 0, (float)m_width, (float)m_height, 0, 1 };
    D3D12_RECT sr = { 0, 0, m_width, m_height };
    cmdList->RSSetViewports(1, &vp);
    cmdList->RSSetScissorRects(1, &sr);

    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle =
        m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    cmdList->OMSetRenderTargets(0, nullptr, FALSE, &dsvHandle);

    // Bind per-frame VP
    XMMATRIX vpT = XMMatrixTranspose(viewProj);
    cmdList->SetGraphicsRoot32BitConstants(0, 16, &vpT, 0);

    for (const auto& e : entities) {
        if (!e.mesh) continue;
        if (e.mesh->vertexBufferId >= m_vertexBuffers.size()) continue;
        if (e.mesh->indexBufferId >= m_indexBuffers.size()) continue;

        XMMATRIX worldT = XMMatrixTranspose(e.transform.GetMatrix());
        cmdList->SetGraphicsRoot32BitConstants(1, 16, &worldT, 0);

        cmdList->IASetVertexBuffers(0, 1,
            &m_vertexBuffers[e.mesh->vertexBufferId].view);
        cmdList->IASetIndexBuffer(
            &m_indexBuffers[e.mesh->indexBufferId].view);
        cmdList->DrawIndexedInstanced(e.mesh->indexCount, 1, 0, 0, 0);
    }
}

uint32_t RenderPipeline::CreateVertexBuffer(const std::vector<SceneVertex>& vertices) {
    VBEntry entry;

    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = static_cast<UINT64>(vertices.size() * sizeof(SceneVertex));
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.SampleDesc.Count = 1;

    HRESULT hr = m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE,
        &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&entry.resource));
    if (FAILED(hr)) return UINT32_MAX;

    void* mapped;
    entry.resource->Map(0, nullptr, &mapped);
    memcpy(mapped, vertices.data(), vertices.size() * sizeof(SceneVertex));
    entry.resource->Unmap(0, nullptr);

    entry.view.BufferLocation = entry.resource->GetGPUVirtualAddress();
    entry.view.StrideInBytes = sizeof(SceneVertex);
    entry.view.SizeInBytes = static_cast<UINT>(vertices.size() * sizeof(SceneVertex));

    uint32_t id = (uint32_t)m_vertexBuffers.size();
    m_vertexBuffers.push_back(std::move(entry));
    return id;
}

uint32_t RenderPipeline::CreateIndexBuffer(const std::vector<uint32_t>& indices) {
    IBEntry entry;

    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = static_cast<UINT64>(indices.size() * sizeof(uint32_t));
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.SampleDesc.Count = 1;

    HRESULT hr = m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE,
        &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&entry.resource));
    if (FAILED(hr)) return UINT32_MAX;

    void* mapped;
    entry.resource->Map(0, nullptr, &mapped);
    memcpy(mapped, indices.data(), indices.size() * sizeof(uint32_t));
    entry.resource->Unmap(0, nullptr);

    entry.view.BufferLocation = entry.resource->GetGPUVirtualAddress();
    entry.view.SizeInBytes = static_cast<UINT>(indices.size() * sizeof(uint32_t));
    entry.view.Format = DXGI_FORMAT_R32_UINT;

    uint32_t id = (uint32_t)m_indexBuffers.size();
    m_indexBuffers.push_back(std::move(entry));
    return id;
}

void RenderPipeline::Shutdown() {
    m_vertexBuffers.clear();
    m_indexBuffers.clear();
    m_depthTexture.Reset();
    m_dsvHeap.Reset();
    m_pipelineState.Reset();
    m_rootSignature.Reset();
    m_device = nullptr;
}

} // namespace

``

## src/wallpaper/renderer/RenderPipeline.hpp

``cpp
#pragma once
#include <d3d12.h>
#include <wrl.h>
#include <DirectXMath.h>
#include <vector>
#include "ECS.hpp"

namespace rex::wallpaper {

using namespace DirectX;

struct SceneVertex {
    float x, y, z;
    float nx, ny, nz;
    float u, v;
};

class RenderPipeline {
public:
    bool Initialize(ID3D12Device* device, int width, int height);
    void Render(ID3D12GraphicsCommandList* cmdList,
                const std::vector<Entity>& entities,
                const XMMATRIX& viewProj,
                const float clearColor[4]);
    void Shutdown();

    uint32_t CreateVertexBuffer(const std::vector<SceneVertex>& vertices);
    uint32_t CreateIndexBuffer(const std::vector<uint32_t>& indices);
    uint32_t GetVertexBufferCount() const { return (uint32_t)m_vertexBuffers.size(); }

    ID3D12Resource* GetDepthBuffer() const { return m_depthTexture.Get(); }
    ID3D12DescriptorHeap* GetDsvHeap() const { return m_dsvHeap.Get(); }

private:
    struct VBEntry {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        D3D12_VERTEX_BUFFER_VIEW view = {};
    };
    struct IBEntry {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        D3D12_INDEX_BUFFER_VIEW view = {};
    };

    ID3D12Device* m_device = nullptr;
    int m_width = 0, m_height = 0;

    std::vector<VBEntry> m_vertexBuffers;
    std::vector<IBEntry> m_indexBuffers;

    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pipelineState;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_depthTexture;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
};

} // namespace

``

## src/wallpaper/renderer/SceneRenderer.cpp

``cpp
#include "SceneRenderer.hpp"
#include "graphics/compositor/compositor.h"
#include <cstdio>

namespace rex::wallpaper {

SceneRenderer::SceneRenderer() = default;
SceneRenderer::~SceneRenderer() { unload(); }

bool SceneRenderer::load(const SceneInfo& scene, Compositor& compositor) {
    m_path = scene.sourcePath;
    m_width  = static_cast<int>(compositor.GetViewportWidth());
    m_height = static_cast<int>(compositor.GetViewportHeight());

    printf("[SceneRenderer] loading scene: %s (%dx%d)\n",
           m_path.c_str(), m_width, m_height);

    m_compositor = &compositor;
    if (!InitD3D12(compositor, m_width, m_height)) return false;

    if (!m_loader.Load(m_path, m_ecs, m_pipeline)) {
        printf("[SceneRenderer] failed to load scene\n");
        return false;
    }

    compositor.SubmitLayer({ SCENE_LAYER_ID, 0, nullptr,
        { 0, 0, (float)m_width, (float)m_height },
        1.0f, 0, true });

    m_playing.store(true);
    printf("[SceneRenderer] loaded (%zu entities)\n", m_ecs.GetEntities().size());
    return true;
}

void SceneRenderer::update(double deltaTime) {
    m_time += deltaTime;
    UpdateCamera(deltaTime);
}

void SceneRenderer::render(Compositor& compositor) {
    if (!m_playing.load() || !m_device) return;

    DrawFrame();

    m_fenceValue++;
    m_commandQueue->Signal(m_sharedFence.Get(), m_fenceValue);
    compositor.WaitForExternalFence(m_fenceHandle, m_fenceValue);
}

void SceneRenderer::unload() {
    m_playing.store(false);

    if (m_commandQueue && m_sharedFence) {
        m_fenceValue++;
        m_commandQueue->Signal(m_sharedFence.Get(), m_fenceValue);
        HANDLE ev = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (ev) {
            m_sharedFence->SetEventOnCompletion(m_fenceValue, ev);
            WaitForSingleObject(ev, INFINITE);
            CloseHandle(ev);
        }
    }

    if (m_compositor) {
        m_compositor->RemoveLayerTexture(SCENE_LAYER_ID);
        m_compositor->RemoveLayer(SCENE_LAYER_ID);
        m_compositor = nullptr;
    }

    m_pipeline.Shutdown();

    m_sharedTexture.Reset();
    m_rtvHeap.Reset();
    m_sharedFence.Reset();
    if (m_fenceHandle) { CloseHandle(m_fenceHandle); m_fenceHandle = nullptr; }
    m_commandList.Reset();
    m_commandAllocator.Reset();
    m_commandQueue.Reset();
    m_device.Reset();
}

bool SceneRenderer::InitD3D12(Compositor& compositor, int width, int height) {
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0,
        IID_PPV_ARGS(&m_device)))) {
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_commandQueue))))
        return false;

    // Shared texture
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = (UINT64)width;
    td.Height = (UINT)height;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
             | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

    D3D12_CLEAR_VALUE cv = {};
    cv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    cv.Color[0] = 0; cv.Color[1] = 0; cv.Color[2] = 0; cv.Color[3] = 1;

    if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED,
        &td, D3D12_RESOURCE_STATE_COMMON, &cv,
        IID_PPV_ARGS(&m_sharedTexture)))) {
        return false;
    }

    HANDLE sharedHandle = nullptr;
    if (FAILED(m_device->CreateSharedHandle(m_sharedTexture.Get(), nullptr,
        GENERIC_ALL, L"REXSceneFrame", &sharedHandle))) {
        return false;
    }
    compositor.ImportLayerTexture(SCENE_LAYER_ID, sharedHandle, width, height);
    CloseHandle(sharedHandle);

    // Shared fence
    if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_SHARED,
        IID_PPV_ARGS(&m_sharedFence)))) {
        return false;
    }
    if (FAILED(m_device->CreateSharedHandle(m_sharedFence.Get(), nullptr,
        GENERIC_ALL, L"REXSceneFence", &m_fenceHandle))) {
        return false;
    }
    m_fenceValue = 0;

    // RTV heap
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
    rtvDesc.NumDescriptors = 1;
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    if (FAILED(m_device->CreateDescriptorHeap(&rtvDesc,
        IID_PPV_ARGS(&m_rtvHeap)))) {
        return false;
    }
    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_RENDER_TARGET_VIEW_DESC rtv = {};
    rtv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    m_device->CreateRenderTargetView(m_sharedTexture.Get(), &rtv,
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart());

    // Command list
    if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&m_commandAllocator)))) return false;
    if (FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_commandAllocator.Get(), nullptr, IID_PPV_ARGS(&m_commandList)))) return false;
    m_commandList->Close();

    // Initialize pipeline (creates the depth buffer too)
    if (!m_pipeline.Initialize(m_device.Get(), width, height)) return false;

    printf("[SceneRenderer] D3D12 initialized\n");
    return true;
}

void SceneRenderer::UpdateCamera(double dt) {
    m_cameraAngle += (float)dt * 0.3f;
    // Find active camera entity and orbit it
    for (auto& e : m_ecs.GetEntities()) {
        if (e.camera && e.camera->active) {
            float x = sinf(m_cameraAngle) * m_orbitRadius;
            float z = cosf(m_cameraAngle) * m_orbitRadius;
            e.transform.position = XMFLOAT3(x, 2.0f, z);
            break;
        }
    }
}

void SceneRenderer::DrawFrame() {
    m_commandAllocator->Reset();
    m_commandList->Reset(m_commandAllocator.Get(), nullptr);

    // Barrier: COMMON → RENDER_TARGET
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_sharedTexture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    m_commandList->ResourceBarrier(1, &barrier);

    // Bind RTV + DSV (pipeline owns the depth buffer)
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = m_pipeline.GetDsvHeap()
        ->GetCPUDescriptorHandleForHeapStart();
    m_commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

    const float clearColor[] = { 0.1f, 0.1f, 0.15f, 1.0f };
    m_commandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
    m_commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH,
        1.0f, 0, 0, nullptr);

    // Set viewport
    D3D12_VIEWPORT vp = { 0, 0, (float)m_width, (float)m_height, 0, 1 };
    D3D12_RECT sr = { 0, 0, m_width, m_height };
    m_commandList->RSSetViewports(1, &vp);
    m_commandList->RSSetScissorRects(1, &sr);

    // Compute view-proj
    XMMATRIX view = XMMatrixIdentity();
    XMMATRIX proj = XMMatrixPerspectiveFovLH(
        XM_PIDIV4, (float)m_width / (float)m_height, 0.1f, 1000.0f);

    auto* cam = m_ecs.GetActiveCamera();
    if (cam) {
        for (auto& e : m_ecs.GetEntities()) {
            if (e.camera.get() == cam) {
                XMVECTOR eye = XMLoadFloat3(&e.transform.position);
                XMVECTOR at = XMVectorZero();
                XMVECTOR up = XMVectorSet(0,1,0,0);
                view = XMMatrixLookAtLH(eye, at, up);
                proj = XMMatrixPerspectiveFovLH(
                    cam->fov, (float)m_width / (float)m_height,
                    cam->nearPlane, cam->farPlane);
                break;
            }
        }
    }
    XMMATRIX viewProj = XMMatrixMultiply(view, proj);

    // Render
    m_pipeline.Render(m_commandList.Get(), m_ecs.GetEntities(), viewProj, clearColor);

    // Barrier: RENDER_TARGET → COMMON
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    m_commandList->ResourceBarrier(1, &barrier);

    m_commandList->Close();

    ID3D12CommandList* lists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);
}

} // namespace

``

## src/wallpaper/renderer/SceneRenderer.hpp

``cpp
#pragma once
#include "IRenderer.hpp"
#include "ECS.hpp"
#include "RenderPipeline.hpp"
#include "SceneLoader.hpp"
#include <d3d12.h>
#include <wrl.h>
#include <string>
#include <atomic>

class Compositor;

namespace rex::wallpaper {

class SceneRenderer : public IRenderer {
public:
    SceneRenderer();
    ~SceneRenderer() override;

    bool load(const SceneInfo& scene, Compositor& compositor) override;
    void update(double deltaTime) override;
    void render(Compositor& compositor) override;
    void unload() override;

    void play() override { m_playing.store(true); }
    void pause() override { m_playing.store(false); }
    void stop() override { m_playing.store(false); }
    bool isPlaying() const override { return m_playing.load(); }

private:
    bool InitD3D12(Compositor& compositor, int width, int height);
    void UpdateCamera(double dt);
    void DrawFrame();

    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_commandQueue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_commandAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commandList;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_sharedTexture;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_sharedFence;
    HANDLE m_fenceHandle = nullptr;
    UINT64 m_fenceValue = 1;
    UINT m_rtvDescriptorSize = 0;

    ECS m_ecs;
    RenderPipeline m_pipeline;
    SceneLoader m_loader;
    float m_cameraAngle = 0.0f;
    float m_orbitRadius = 5.0f;
    double m_time = 0.0;

    int m_width = 0, m_height = 0;
    std::atomic<bool> m_playing{false};
    std::string m_path;
    Compositor* m_compositor = nullptr;
    static constexpr uint64_t SCENE_LAYER_ID = 3;
};

} // namespace

``

## src/wallpaper/renderer/SceneLoader.cpp

``cpp
#include "SceneLoader.hpp"
#include <cstdio>
#include <vector>
#include <filesystem>

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

namespace rex::wallpaper {

bool SceneLoader::Load(const std::string& path, ECS& ecs, RenderPipeline& pipeline) {
    if (!std::filesystem::exists(path)) {
        printf("[SceneLoader] File not found: %s\n", path.c_str());
        return LoadBuiltinScene(ecs, pipeline);
    }

    // Try glTF
    cgltf_options opts = {};
    cgltf_data* data = nullptr;
    cgltf_result result = cgltf_parse_file(&opts, path.c_str(), &data);
    if (result != cgltf_result_success) {
        printf("[SceneLoader] cgltf_parse_file failed: %d\n", result);
        return LoadBuiltinScene(ecs, pipeline);
    }

    result = cgltf_load_buffers(&opts, data, path.c_str());
    if (result != cgltf_result_success) {
        printf("[SceneLoader] cgltf_load_buffers failed: %d\n", result);
        cgltf_free(data);
        return LoadBuiltinScene(ecs, pipeline);
    }

    cgltf_scene* scene = data->scene;
    if (!scene && data->scenes_count > 0)
        scene = &data->scenes[0];
    if (!scene) {
        printf("[SceneLoader] no scene found\n");
        cgltf_free(data);
        return LoadBuiltinScene(ecs, pipeline);
    }

    auto processNode = [&](cgltf_node* node, auto&& self) -> void {
        // Build transform
        XMFLOAT3 pos = {0,0,0}, scl = {1,1,1};
        if (node->has_translation) { pos.x = node->translation[0]; pos.y = node->translation[1]; pos.z = node->translation[2]; }
        if (node->has_scale) { scl.x = node->scale[0]; scl.y = node->scale[1]; scl.z = node->scale[2]; }

        if (node->mesh) {
            for (size_t p = 0; p < node->mesh->primitives_count; ++p) {
                auto& prim = node->mesh->primitives[p];
                if (!prim.indices || prim.type != cgltf_primitive_type_triangles) continue;

                const cgltf_accessor* posAcc = cgltf_find_accessor(&prim, cgltf_attribute_type_position, 0);
                const cgltf_accessor* nrmAcc = cgltf_find_accessor(&prim, cgltf_attribute_type_normal, 0);
                const cgltf_accessor* uvAcc  = cgltf_find_accessor(&prim, cgltf_attribute_type_texcoord, 0);
                if (!posAcc || !prim.indices) continue;

                // Read vertices
                size_t vertCount = posAcc->count;
                std::vector<SceneVertex> verts(vertCount);
                for (size_t i = 0; i < vertCount; ++i) {
                    cgltf_accessor_read_float(posAcc, i, &verts[i].x, 3);
                    if (nrmAcc) cgltf_accessor_read_float(nrmAcc, i, &verts[i].nx, 3);
                    else { verts[i].nx = 0; verts[i].ny = 1; verts[i].nz = 0; }
                    if (uvAcc) cgltf_accessor_read_float(uvAcc, i, &verts[i].u, 2);
                    else { verts[i].u = 0; verts[i].v = 0; }
                }

                // Read indices
                size_t idxCount = prim.indices->count;
                std::vector<uint32_t> indices(idxCount);
                for (size_t i = 0; i < idxCount; ++i)
                    indices[i] = (uint32_t)cgltf_accessor_read_index(prim.indices, i);

                uint32_t vbId = pipeline.CreateVertexBuffer(verts);
                uint32_t ibId = pipeline.CreateIndexBuffer(indices);

                Entity* entity = ecs.CreateEntity();
                entity->mesh = std::make_unique<MeshComponent>();
                entity->mesh->vertexBufferId = vbId;
                entity->mesh->indexBufferId = ibId;
                entity->mesh->indexCount = (uint32_t)idxCount;

                // Apply node transform
                XMFLOAT3 rot = {0,0,0};
                if (node->has_rotation) {
                    float qx=node->rotation[0], qy=node->rotation[1],
                          qz=node->rotation[2], qw=node->rotation[3];
                    float sr_cp = 2*(qw*qx + qy*qz);
                    float cr_cp = 1 - 2*(qx*qx + qy*qy);
                    rot.x = atan2f(sr_cp, cr_cp);
                    float sp = 2*(qw*qy - qz*qx);
                    rot.y = fabsf(sp) >= 1 ? copysignf(1.570796f, sp) : asinf(sp);
                    float sy_cp = 2*(qw*qz + qx*qy);
                    float cy_cp = 1 - 2*(qy*qy + qz*qz);
                    rot.z = atan2f(sy_cp, cy_cp);
                }
                entity->transform.position = pos;
                entity->transform.rotation = rot;
                entity->transform.scale = scl;
            }
        }

        // Add camera
        if (node->camera && node->camera->type == cgltf_camera_type_perspective) {
            Entity* camEnt = ecs.CreateEntity();
            camEnt->camera = std::make_unique<CameraComponent>();
            camEnt->camera->active = true;
            camEnt->camera->fov = node->camera->data.perspective.yfov;
            camEnt->camera->nearPlane = node->camera->data.perspective.znear;
            camEnt->camera->farPlane = node->camera->data.perspective.has_zfar
                ? node->camera->data.perspective.zfar : 1000.0f;
            camEnt->transform.position = pos;
            XMFLOAT3 rot = {0,0,0};
            if (node->has_rotation) {
                float qx=node->rotation[0], qy=node->rotation[1],
                      qz=node->rotation[2], qw=node->rotation[3];
                float sr_cp = 2*(qw*qx + qy*qz);
                float cr_cp = 1 - 2*(qx*qx + qy*qy);
                rot.x = atan2f(sr_cp, cr_cp);
                float sp = 2*(qw*qy - qz*qx);
                rot.y = fabsf(sp) >= 1 ? copysignf(1.570796f, sp) : asinf(sp);
                float sy_cp = 2*(qw*qz + qx*qy);
                float cy_cp = 1 - 2*(qy*qy + qz*qz);
                rot.z = atan2f(sy_cp, cy_cp);
            }
            camEnt->transform.rotation = rot;
        }

        for (size_t i = 0; i < node->children_count; ++i)
            self(node->children[i], self);
    };

    for (size_t i = 0; i < scene->nodes_count; ++i)
        processNode(scene->nodes[i], processNode);

    // Default camera
    if (!ecs.GetActiveCamera()) {
        Entity* camEnt = ecs.CreateEntity();
        camEnt->camera = std::make_unique<CameraComponent>();
        camEnt->camera->active = true;
        camEnt->transform.position = XMFLOAT3(0, 2, -5);
    }

    cgltf_free(data);
    printf("[SceneLoader] loaded %zu entities\n", ecs.GetEntities().size());
    return true;
}

static const float CUBE_VERTICES[] = {
    // positions          normals          uvs
    -1,-1,-1, 0,0,-1, 0,1,
     1,-1,-1, 0,0,-1, 1,1,
    -1, 1,-1, 0,0,-1, 0,0,
     1, 1,-1, 0,0,-1, 1,0,
    -1,-1, 1, 0,0, 1, 0,1,
     1,-1, 1, 0,0, 1, 1,1,
    -1, 1, 1, 0,0, 1, 0,0,
     1, 1, 1, 0,0, 1, 1,0,
    -1,-1,-1,-1,0,0, 0,1,
    -1, 1,-1,-1,0,0, 0,0,
    -1,-1, 1,-1,0,0, 1,1,
    -1, 1, 1,-1,0,0, 1,0,
     1,-1,-1, 1,0,0, 0,1,
     1, 1,-1, 1,0,0, 0,0,
     1,-1, 1, 1,0,0, 1,1,
     1, 1, 1, 1,0,0, 1,0,
    -1,-1,-1, 0,-1,0, 0,1,
     1,-1,-1, 0,-1,0, 1,1,
    -1,-1, 1, 0,-1,0, 0,0,
     1,-1, 1, 0,-1,0, 1,0,
    -1, 1,-1, 0,1,0, 0,0,
     1, 1,-1, 0,1,0, 1,0,
    -1, 1, 1, 0,1,0, 0,1,
     1, 1, 1, 0,1,0, 1,1,
};

static const uint32_t CUBE_INDICES[] = {
    0,1,2, 1,3,2,
    4,6,5, 5,6,7,
    8,10,9, 9,10,11,
    12,13,14, 13,15,14,
    16,17,18, 17,19,18,
    20,22,21, 21,22,23,
};

bool SceneLoader::LoadBuiltinScene(ECS& ecs, RenderPipeline& pipeline) {
    printf("[SceneLoader] loading built-in cube\n");

    size_t vertCount = sizeof(CUBE_VERTICES) / (8 * sizeof(float));
    std::vector<SceneVertex> verts(vertCount);
    for (size_t i = 0; i < vertCount; ++i) {
        verts[i].x  = CUBE_VERTICES[i*8+0];
        verts[i].y  = CUBE_VERTICES[i*8+1];
        verts[i].z  = CUBE_VERTICES[i*8+2];
        verts[i].nx = CUBE_VERTICES[i*8+3];
        verts[i].ny = CUBE_VERTICES[i*8+4];
        verts[i].nz = CUBE_VERTICES[i*8+5];
        verts[i].u  = CUBE_VERTICES[i*8+6];
        verts[i].v  = CUBE_VERTICES[i*8+7];
    }

    size_t idxCount = sizeof(CUBE_INDICES) / sizeof(uint32_t);
    std::vector<uint32_t> indices(CUBE_INDICES, CUBE_INDICES + idxCount);

    uint32_t vbId = pipeline.CreateVertexBuffer(verts);
    uint32_t ibId = pipeline.CreateIndexBuffer(indices);

    Entity* ent = ecs.CreateEntity();
    ent->mesh = std::make_unique<MeshComponent>();
    ent->mesh->vertexBufferId = vbId;
    ent->mesh->indexBufferId = ibId;
    ent->mesh->indexCount = (uint32_t)idxCount;

    // Default camera
    Entity* camEnt = ecs.CreateEntity();
    camEnt->camera = std::make_unique<CameraComponent>();
    camEnt->camera->active = true;
    camEnt->transform.position = XMFLOAT3(0, 2, -5);

    printf("[SceneLoader] built-in cube loaded (%zu verts, %zu indices)\n",
           vertCount, idxCount);
    return true;
}

} // namespace

``

## src/wallpaper/renderer/SceneLoader.hpp

``cpp
#pragma once
#include <string>
#include "ECS.hpp"
#include "RenderPipeline.hpp"

namespace rex::wallpaper {

class SceneLoader {
public:
    bool Load(const std::string& path, ECS& ecs, RenderPipeline& pipeline);

private:
    bool LoadBuiltinScene(ECS& ecs, RenderPipeline& pipeline);
};

} // namespace

``

## src/wallpaper/renderer/ShaderRenderer.cpp

``cpp
#include "ShaderRenderer.hpp"
#include "graphics/compositor/compositor.h"
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <fstream>
#include <sstream>
#include <d3dcompiler.h>


namespace rex::wallpaper {

struct ShaderCB {
    float time, sinTime, cosTime, pad0;
    float width, height, invWidth, invHeight;
};

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

ShaderRenderer::ShaderRenderer() = default;
ShaderRenderer::~ShaderRenderer() { unload(); }

bool ShaderRenderer::load(const SceneInfo& scene, ::Compositor& compositor) {
    m_path = scene.sourcePath;
    if (m_path.empty()) return false;

    std::string hlsl;
    std::ifstream file(m_path);
    if (file.good()) {
        std::stringstream buf;
        buf << file.rdbuf();
        hlsl = buf.str();
    } else {
        hlsl = m_path;
    }
    if (hlsl.empty()) return false;

    m_compositor = &compositor;
    m_width = static_cast<int>(compositor.GetViewportWidth());
    m_height = static_cast<int>(compositor.GetViewportHeight());

    if (!InitD3D12()) { Log("[ShaderRenderer] D3D12 init failed\n"); return false; }
    if (!CompileShaders(hlsl)) { Log("[ShaderRenderer] shader compile failed\n"); return false; }
    if (!CreateRootSignature()) { Log("[ShaderRenderer] root sig failed\n"); return false; }
    if (!CreatePipelineState()) { Log("[ShaderRenderer] PSO failed\n"); return false; }
    if (!CreateSharedResources(m_width, m_height)) { Log("[ShaderRenderer] shared resources failed\n"); return false; }

    compositor.SubmitLayer({ LAYER_ID, 0, nullptr,
        { 0, 0, static_cast<float>(m_width), static_cast<float>(m_height) },
        1.0f, 0, true });

    m_playing.store(true);
    Log("[ShaderRenderer] loaded (%dx%d)\n", m_width, m_height);
    return true;
}

void ShaderRenderer::update(double deltaTime) {
    m_time += deltaTime;
}

void ShaderRenderer::render(::Compositor& compositor) {
    if (!m_playing.load() || !m_device) return;

    // update constant buffer
    {
        ShaderCB data;
        data.time = static_cast<float>(m_time);
        data.sinTime = sinf(data.time);
        data.cosTime = cosf(data.time);
        data.pad0 = 0.0f;
        data.width = static_cast<float>(m_width);
        data.height = static_cast<float>(m_height);
        data.invWidth = 1.0f / data.width;
        data.invHeight = 1.0f / data.height;

        void* mapped;
        m_constantBuffer->Map(0, nullptr, &mapped);
        memcpy(mapped, &data, sizeof(data));
        m_constantBuffer->Unmap(0, nullptr);
    }

    Draw();

    // signal fence for compositor
    m_fenceVal++;
    m_cmdQueue->Signal(m_sharedFence.Get(), m_fenceVal);
    compositor.WaitForExternalFence(m_fenceHandle, m_fenceVal);
}

void ShaderRenderer::unload() {
    m_playing.store(false);

    // wait for GPU
    if (m_cmdQueue && m_sharedFence) {
        m_fenceVal++;
        m_cmdQueue->Signal(m_sharedFence.Get(), m_fenceVal);

        HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (event) {
            m_sharedFence->SetEventOnCompletion(m_fenceVal, event);
            WaitForSingleObject(event, INFINITE);
            CloseHandle(event);
        }
    }

    if (m_compositor) {
        m_compositor->RemoveLayerTexture(LAYER_ID);
        m_compositor->RemoveLayer(LAYER_ID);
        m_compositor = nullptr;
    }

    m_sharedTexture.Reset();
    m_constantBuffer.Reset();
    m_sharedFence.Reset();
    if (m_fenceHandle) { CloseHandle(m_fenceHandle); m_fenceHandle = nullptr; }
    m_rtvHeap.Reset();
    m_pipelineState.Reset();
    m_rootSignature.Reset();
    m_cmdList.Reset();
    m_cmdAllocator.Reset();
    m_cmdQueue.Reset();
    m_device.Reset();
}

void ShaderRenderer::Draw() {
    m_cmdAllocator->Reset();
    m_cmdList->Reset(m_cmdAllocator.Get(), m_pipelineState.Get());

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    m_cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    float clear[4] = { 0, 0, 0, 1 };
    m_cmdList->ClearRenderTargetView(rtv, clear, 0, nullptr);

    m_cmdList->SetGraphicsRootSignature(m_rootSignature.Get());
    m_cmdList->SetGraphicsRootConstantBufferView(0, m_constantBuffer->GetGPUVirtualAddress());

    D3D12_VIEWPORT vp = { 0, 0, static_cast<float>(m_width), static_cast<float>(m_height), 0, 1 };
    D3D12_RECT sr = { 0, 0, m_width, m_height };
    m_cmdList->RSSetViewports(1, &vp);
    m_cmdList->RSSetScissorRects(1, &sr);

    m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_cmdList->DrawInstanced(3, 1, 0, 0);

    m_cmdList->Close();

    ID3D12CommandList* lists[] = { m_cmdList.Get() };
    m_cmdQueue->ExecuteCommandLists(1, lists);
}

bool ShaderRenderer::InitD3D12() {
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device)))) {
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    if (FAILED(m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_cmdQueue)))) return false;

    if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&m_cmdAllocator)))) return false;

    if (FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_cmdAllocator.Get(), nullptr, IID_PPV_ARGS(&m_cmdList)))) return false;
    m_cmdList->Close();

    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
    rtvDesc.NumDescriptors = 1;
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    if (FAILED(m_device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m_rtvHeap)))) return false;
    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    return true;
}

bool ShaderRenderer::CompileShaders(const std::string& hlslSource) {
    const char* vsSrc =
        "struct VSOutput { float4 pos : SV_Position; float2 uv : TEXCOORD; };"
        "VSOutput mainVS(uint id : SV_VertexID) {"
        "    float2 pos[3] = { float2(-1,-1), float2(3,-1), float2(-1,3) };"
        "    VSOutput o;"
        "    o.pos = float4(pos[id], 0, 1);"
        "    o.uv = float2(o.pos.x * 0.5 + 0.5, 1.0 - (o.pos.y * 0.5 + 0.5));"
        "    return o;"
        "}";

    ComPtr<ID3DBlob> err;
    if (FAILED(D3DCompile(vsSrc, strlen(vsSrc), nullptr, nullptr, nullptr,
        "mainVS", "vs_5_0", 0, 0, &m_vsBlob, &err))) {
        Log("[ShaderRenderer] VS compile failed: %s\n",
            err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }

    if (FAILED(D3DCompile(hlslSource.c_str(), hlslSource.size(), nullptr, nullptr, nullptr,
        "mainPS", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &m_psBlob, &err))) {
        Log("[ShaderRenderer] PS compile failed: %s\n",
            err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }

    return true;
}

bool ShaderRenderer::CreateRootSignature() {
    D3D12_ROOT_PARAMETER param = {};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    param.Descriptor.ShaderRegister = 0;
    param.Descriptor.RegisterSpace = 0;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 1;
    desc.pParameters = &param;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
        &sig, &err))) {
        Log("[ShaderRenderer] serialize root sig failed\n");
        return false;
    }
    if (FAILED(m_device->CreateRootSignature(0, sig->GetBufferPointer(),
        sig->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature)))) {
        return false;
    }
    return true;
}

bool ShaderRenderer::CreatePipelineState() {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = m_rootSignature.Get();
    pso.VS = { m_vsBlob->GetBufferPointer(), m_vsBlob->GetBufferSize() };
    pso.PS = { m_psBlob->GetBufferPointer(), m_psBlob->GetBufferSize() };
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.BlendState.RenderTarget[0].BlendEnable = FALSE;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.DSVFormat = DXGI_FORMAT_UNKNOWN;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.SampleDesc.Count = 1;

    if (FAILED(m_device->CreateGraphicsPipelineState(&pso,
        IID_PPV_ARGS(&m_pipelineState)))) {
        return false;
    }
    return true;
}

bool ShaderRenderer::CreateSharedResources(int width, int height) {
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = static_cast<UINT64>(width);
    td.Height = static_cast<UINT>(height);
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
             | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

    D3D12_CLEAR_VALUE cv = {};
    cv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    cv.Color[0] = 0; cv.Color[1] = 0; cv.Color[2] = 0; cv.Color[3] = 1;

    if (FAILED(m_device->CreateCommittedResource(&hp,
        D3D12_HEAP_FLAG_SHARED, &td,
        D3D12_RESOURCE_STATE_COMMON, &cv,
        IID_PPV_ARGS(&m_sharedTexture)))) {
        return false;
    }

    HANDLE sharedHandle = nullptr;
    if (FAILED(m_device->CreateSharedHandle(m_sharedTexture.Get(), nullptr,
        GENERIC_ALL, L"REXShaderFrame", &sharedHandle))) {
        return false;
    }

    m_compositor->ImportLayerTexture(LAYER_ID, sharedHandle, width, height);
    CloseHandle(sharedHandle);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    m_device->CreateRenderTargetView(m_sharedTexture.Get(), nullptr, rtv);

    if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_SHARED,
        IID_PPV_ARGS(&m_sharedFence)))) {
        return false;
    }
    if (FAILED(m_device->CreateSharedHandle(m_sharedFence.Get(), nullptr,
        GENERIC_ALL, L"REXShaderFence", &m_fenceHandle))) {
        return false;
    }
    m_fenceVal = 0;

    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 256;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.SampleDesc.Count = 1;

    if (FAILED(m_device->CreateCommittedResource(&hp,
        D3D12_HEAP_FLAG_NONE, &bd,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_constantBuffer)))) {
        return false;
    }

    return true;
}

} // namespace

``

## src/wallpaper/renderer/ShaderRenderer.hpp

``cpp
#pragma once
#include "IRenderer.hpp"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl.h>
#include <string>
#include <atomic>

namespace rex::wallpaper {

class ShaderRenderer : public IRenderer {
public:
    ShaderRenderer();
    ~ShaderRenderer() override;

    bool load(const SceneInfo& scene, ::Compositor& compositor) override;
    void update(double deltaTime) override;
    void render(::Compositor& compositor) override;
    void unload() override;

    void play() override { m_playing.store(true); }
    void pause() override { m_playing.store(false); }
    void stop() override { m_playing.store(false); }
    bool isPlaying() const override { return m_playing.load(); }

private:
    bool InitD3D12();
    bool CompileShaders(const std::string& hlslSource);
    bool CreateRootSignature();
    bool CreatePipelineState();
    bool CreateSharedResources(int width, int height);
    void Draw();

    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_cmdQueue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_cmdAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_cmdList;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pipelineState;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_sharedTexture;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_constantBuffer;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_sharedFence;
    HANDLE m_fenceHandle = nullptr;
    UINT64 m_fenceVal = 0;
    UINT m_rtvDescriptorSize = 0;

    Microsoft::WRL::ComPtr<ID3DBlob> m_vsBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> m_psBlob;

    ::Compositor* m_compositor = nullptr;
    int m_width = 0;
    int m_height = 0;
    std::string m_path;
    std::atomic<bool> m_playing{false};
    double m_time = 0.0;

    static constexpr uint64_t LAYER_ID = 2;
};

} // namespace

``

## src/wallpaper/renderer/ImageRenderer.cpp

``cpp
#include "ImageRenderer.hpp"
#include "graphics/compositor/compositor.h"
#include <cstdio>

namespace rex::wallpaper {

bool ImageRenderer::load(const SceneInfo& scene, Compositor& compositor) {
    m_path = scene.sourcePath;
    if (m_path.empty()) {
        printf("[ImageRenderer] Empty path\n");
        return false;
    }
    printf("[ImageRenderer] Loading: %s\n", m_path.c_str());
    return compositor.SetBackgroundFromFile(m_path.c_str());
}

} // namespace

``

## src/wallpaper/renderer/ImageRenderer.hpp

``cpp
#pragma once
#include "IRenderer.hpp"
#include <string>

namespace rex::wallpaper {

class ImageRenderer : public IRenderer {
public:
    bool load(const SceneInfo& scene, Compositor& compositor) override;
    void update(double) override {}
    void render(Compositor&) override {}
    void unload() override {}

private:
    std::string m_path;
};

} // namespace

``

## src/wallpaper/renderer/AudioVisualizerRenderer.cpp

``cpp
#include "AudioVisualizerRenderer.hpp"
#include "graphics/compositor/compositor.h"
#include <cstdio>
#include <cmath>
#include <d3dcompiler.h>
#include <avrt.h>
#include <algorithm>

namespace {

// Real Cooley-Tukey FFT in-place
void Fft(std::vector<float>& re, std::vector<float>& im) {
    int n = (int)re.size();
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -6.283185307f / len;
        float wlen_r = cosf(ang), wlen_i = sinf(ang);
        for (int i = 0; i < n; i += len) {
            float w_r = 1.0f, w_i = 0.0f;
            for (int j = 0; j < len / 2; ++j) {
                float u_r = re[i + j], u_i = im[i + j];
                float v_r = re[i + j + len/2] * w_r - im[i + j + len/2] * w_i;
                float v_i = re[i + j + len/2] * w_i + im[i + j + len/2] * w_r;
                re[i + j] = u_r + v_r; im[i + j] = u_i + v_i;
                re[i + j + len/2] = u_r - v_r; im[i + j + len/2] = u_i - v_i;
                float t = w_r * wlen_r - w_i * wlen_i;
                w_i = w_r * wlen_i + w_i * wlen_r;
                w_r = t;
            }
        }
    }
}

// Inline HLSL shaders
const char* g_vsSrc = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut mainVS(uint id : SV_VertexID) {
    VSOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2,-2) + float2(-1,1), 0, 1);
    return o;
}
)";

const char* g_psSrc = R"(
cbuffer C : register(b0) {
    float4 uTimeRes;   // (time, resX, resY, mode)
    float4 uNumBars;   // (numBars, 0,0,0)
    float4 uBars[8];   // 32 spectrum values packed as float4
};
struct PSIn { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

float4 mainPS(PSIn i) : SV_Target {
    float t = uTimeRes.x;
    float2 res = uTimeRes.yz;
    float mode = uTimeRes.w;
    int nb = (int)uNumBars.x;
    float ar = res.x / res.y;
    float2 uv = i.uv;
    uv.x *= ar;

    float bars[32];
    [unroll] for (int k = 0; k < 32; ++k)
        bars[k] = uBars[k/4][k%4];

    // Bars
    if (mode < 0.5f) {
        float bw = ar / nb;
        int idx = (int)(uv.x / bw);
        float lx = frac(uv.x / bw);
        if (idx >= 0 && idx < nb) {
            float h = bars[idx] * 0.8f;
            if (uv.y < h && lx > 0.1f && lx < 0.9f) {
                float3 col = float3(0.1f, 0.3f, 0.5f) + bars[idx] * float3(0.3f, 0.3f, 0.5f);
                return float4(col, 1);
            }
        }
        return float4(0,0,0,1);
    }

    // Waveform
    if (mode < 1.5f) {
        float xp = uv.x / ar;
        int idx = (int)(xp * nb);
        idx = clamp(idx, 0, nb-1);
        float waveY = bars[idx] * 0.4f + 0.5f;
        float d = abs(uv.y - waveY);
        if (d < 0.008f)
            return float4(0.2f, 0.8f, 0.4f, 1);
        return float4(0.02f, 0.02f, 0.03f, 1);
    }

    // Particles
    {
        float3 col = float3(0,0,0);
        for (int i = 0; i < nb; ++i) {
            float a = bars[i] * 6.283f + t;
            float r = 0.08f + bars[i] * 0.12f;
            float2 cp = float2(0.5f*ar, 0.5f);
            float2 pp = cp + float2(cos(a), sin(a)) * r;
            float2 diff = uv - pp;
            float dd = length(diff);
            if (dd < 0.02f) {
                float intensity = bars[i] * 2.0f;
                col += float3(0.3f, 0.1f, 0.6f) * intensity * (1 - dd/0.02f);
            }
        }
        return float4(col, 1);
    }
}
)";

} // anonymous namespace

namespace rex::wallpaper {

AudioVisualizerRenderer::AudioVisualizerRenderer() = default;
AudioVisualizerRenderer::~AudioVisualizerRenderer() { unload(); }

bool AudioVisualizerRenderer::load(const SceneInfo& scene, Compositor& compositor) {
    m_width  = static_cast<int>(compositor.GetViewportWidth());
    m_height = static_cast<int>(compositor.GetViewportHeight());
    m_compositor = &compositor;

    if (scene.type == "audio.waveform") m_visualType = VisualType::Waveform;
    else if (scene.type == "audio.particles") m_visualType = VisualType::Particles;
    else m_visualType = VisualType::Bars;

    if (!InitD3D12(compositor, m_width, m_height)) return false;
    if (!InitAudioCapture()) return false;

    compositor.SubmitLayer({ AV_LAYER_ID, 0, nullptr,
        { 0, 0, (float)m_width, (float)m_height }, 1.0f, 0, true });

    m_playing = true;
    printf("[AudioVisualizer] loaded (mode=%d)\n", (int)m_visualType);
    return true;
}

void AudioVisualizerRenderer::update(double deltaTime) {
    m_time += (float)deltaTime;
}

void AudioVisualizerRenderer::render(Compositor& compositor) {
    if (!m_playing.load() || !m_device) return;

    switch (m_visualType) {
        case VisualType::Bars:       DrawBars();      break;
        case VisualType::Waveform:   DrawWaveform();  break;
        case VisualType::Particles:  DrawParticles(); break;
    }

    m_commandQueue->Signal(m_sharedFence.Get(), m_fenceValue);
    compositor.WaitForExternalFence(m_fenceHandle, m_fenceValue);
}

void AudioVisualizerRenderer::unload() {
    m_playing = false;
    m_audioRunning = false;
    if (m_audioThread.joinable()) m_audioThread.join();

    if (m_audioClient) m_audioClient->Stop();

    if (m_compositor && m_device) {
        m_compositor->RemoveLayerTexture(AV_LAYER_ID);
        m_compositor->RemoveLayer(AV_LAYER_ID);
        m_compositor = nullptr;
    }

    if (m_uploadBuffer && m_mappedSpectrum) {
        m_uploadBuffer->Unmap(0, nullptr);
        m_mappedSpectrum = nullptr;
    }

    m_uploadBuffer.Reset();
    m_psoBars.Reset();
    m_psoWaveform.Reset();
    m_psoParticles.Reset();
    m_rootSig.Reset();
    m_sharedTexture.Reset();
    m_rtvHeap.Reset();
    m_sharedFence.Reset();
    m_commandList.Reset();
    m_commandAllocator.Reset();
    m_commandQueue.Reset();
    m_device.Reset();
    if (m_fenceHandle) { CloseHandle(m_fenceHandle); m_fenceHandle = nullptr; }
    if (m_sharedHandle) { CloseHandle(m_sharedHandle); m_sharedHandle = nullptr; }

    if (m_waveFormat) CoTaskMemFree(m_waveFormat);
    m_audioClient.Reset();
    m_captureClient.Reset();
    m_audioDevice.Reset();
    m_enum.Reset();
    CoUninitialize();
}

// ─── D3D12 Initialization ──────────────────────────────────────────────

bool AudioVisualizerRenderer::InitD3D12(Compositor& compositor, int width, int height) {
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0,
        IID_PPV_ARGS(&m_device)))) return false;

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_commandQueue))))
        return false;

    // Shared texture
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = (UINT64)width;
    td.Height = (UINT)height;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
             | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    D3D12_CLEAR_VALUE cv = {};
    cv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED,
        &td, D3D12_RESOURCE_STATE_COMMON, &cv,
        IID_PPV_ARGS(&m_sharedTexture)))) return false;

    if (FAILED(m_device->CreateSharedHandle(m_sharedTexture.Get(), nullptr,
        GENERIC_ALL, L"REXAVFrame", &m_sharedHandle))) return false;
    compositor.ImportLayerTexture(AV_LAYER_ID, m_sharedHandle, width, height);

    // Fence
    if (FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_SHARED,
        IID_PPV_ARGS(&m_sharedFence)))) return false;
    if (FAILED(m_device->CreateSharedHandle(m_sharedFence.Get(), nullptr,
        GENERIC_ALL, L"REXAVFence", &m_fenceHandle))) return false;

    // RTV
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    if (FAILED(m_device->CreateDescriptorHeap(&rtvDesc,
        IID_PPV_ARGS(&m_rtvHeap)))) return false;
    D3D12_RENDER_TARGET_VIEW_DESC rtv = {};
    rtv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    m_device->CreateRenderTargetView(m_sharedTexture.Get(), &rtv,
        m_rtvHeap->GetCPUDescriptorHandleForHeapStart());

    // Command allocator + list
    if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&m_commandAllocator)))) return false;
    if (FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_commandAllocator.Get(), nullptr, IID_PPV_ARGS(&m_commandList))))
        return false;
    m_commandList->Close();

    // Upload buffer for spectrum (256-byte aligned CB for 32 floats + header)
    UINT cbSize = 256; // 4*8*4 + 4*4 = 144, round to 256
    D3D12_HEAP_PROPERTIES uph = {};
    uph.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC ubd = {};
    ubd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    ubd.Width = cbSize;
    ubd.Height = 1;
    ubd.DepthOrArraySize = 1;
    ubd.MipLevels = 1;
    ubd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(m_device->CreateCommittedResource(&uph, D3D12_HEAP_FLAG_NONE,
        &ubd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_uploadBuffer)))) return false;
    m_uploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&m_mappedSpectrum));

    // Compile shaders
    CompileShaders();

    printf("[AudioVisualizer] D3D12 initialized\n");
    return true;
}

void AudioVisualizerRenderer::CompileShaders() {
    ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;

    auto compile = [&](const char* src, const char* target, ID3DBlob** blob) {
        if (FAILED(D3DCompile(src, strlen(src), nullptr, nullptr, nullptr,
            "main" + 0, target, D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, blob, &errBlob))) {
            if (errBlob) printf("[AV] shader error: %s\n",
                (const char*)errBlob->GetBufferPointer());
            return false;
        }
        return true;
    };

    if (!compile(g_vsSrc, "vs_5_0", vsBlob.GetAddressOf()) ||
        !compile(g_psSrc, "ps_5_0", psBlob.GetAddressOf()))
        return;

    // Root signature: CBV b0 (spectrum data)
    D3D12_ROOT_PARAMETER rp = {};
    rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rp.Descriptor.RegisterSpace = 0;
    rp.Descriptor.ShaderRegister = 0;
    rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC ss = {};
    ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    ss.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss.ShaderRegister = 0;
    ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = 1;
    rsd.pParameters = &rp;
    rsd.NumStaticSamplers = 1;
    rsd.pStaticSamplers = &ss;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> rsBlob;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1,
        rsBlob.GetAddressOf(), &errBlob))) {
        if (errBlob) printf("[AV] root sig error: %s\n",
            (const char*)errBlob->GetBufferPointer());
        return;
    }
    m_device->CreateRootSignature(0, rsBlob->GetBufferPointer(),
        rsBlob->GetBufferSize(), IID_PPV_ARGS(&m_rootSig));

    // PSO
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psd = {};
    psd.pRootSignature = m_rootSig.Get();
    psd.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    psd.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
    psd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psd.NumRenderTargets = 1;
    psd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psd.SampleDesc.Count = 1;

    // All three modes share the same PSO (mode select in CB)
    m_device->CreateGraphicsPipelineState(&psd, IID_PPV_ARGS(&m_psoBars));
    m_device->CreateGraphicsPipelineState(&psd, IID_PPV_ARGS(&m_psoWaveform));
    m_device->CreateGraphicsPipelineState(&psd, IID_PPV_ARGS(&m_psoParticles));
}

// ─── Audio Capture ────────────────────────────────────────────────────

bool AudioVisualizerRenderer::InitAudioCapture() {
    HRESULT hr;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
        CLSCTX_ALL, IID_PPV_ARGS(&m_enum));
    if (FAILED(hr)) { printf("[AV] no MMDeviceEnumerator\n"); return false; }

    hr = m_enum->GetDefaultAudioEndpoint(eRender, eConsole, &m_audioDevice);
    if (FAILED(hr)) { printf("[AV] no default audio endpoint\n"); return false; }

    hr = m_audioDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(m_audioClient.GetAddressOf()));
    if (FAILED(hr)) { printf("[AV] no audio client\n"); return false; }

    hr = m_audioClient->GetMixFormat(&m_waveFormat);
    if (FAILED(hr)) return false;

    REFERENCE_TIME hnsPeriod;
    hr = m_audioClient->GetDevicePeriod(nullptr, &hnsPeriod);
    if (FAILED(hr)) return false;

    hr = m_audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK, hnsPeriod, 0, m_waveFormat, nullptr);
    if (FAILED(hr)) { printf("[AV] initialize failed: %08x\n", hr); return false; }

    hr = m_audioClient->GetService(IID_PPV_ARGS(&m_captureClient));
    if (FAILED(hr)) return false;

    hr = m_audioClient->Start();
    if (FAILED(hr)) return false;

    m_audioRunning = true;
    m_audioThread = std::thread(&AudioVisualizerRenderer::AudioCaptureThread, this);
    printf("[AV] audio capture started\n");
    return true;
}

void AudioVisualizerRenderer::AudioCaptureThread() {
    AvSetMmThreadCharacteristics(L"Audio", nullptr);

    while (m_audioRunning.load()) {
        UINT32 packet = 0;
        if (FAILED(m_captureClient->GetNextPacketSize(&packet))) {
            Sleep(10); continue;
        }
        if (packet == 0) { Sleep(1); continue; }

        BYTE* data;
        UINT32 frames;
        DWORD flags;
        HRESULT hr = m_captureClient->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
        if (FAILED(hr)) continue;

        std::vector<float> mono(frames, 0.0f);
        if (m_waveFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
            float* s = reinterpret_cast<float*>(data);
            for (UINT32 i = 0; i < frames; ++i) {
                float sum = 0;
                for (WORD ch = 0; ch < m_waveFormat->nChannels; ++ch)
                    sum += s[i * m_waveFormat->nChannels + ch];
                mono[i] = sum / m_waveFormat->nChannels;
            }
        } else if (m_waveFormat->wFormatTag == WAVE_FORMAT_PCM ||
                   m_waveFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            int16_t* s = reinterpret_cast<int16_t*>(data);
            for (UINT32 i = 0; i < frames; ++i) {
                int32_t sum = 0;
                for (WORD ch = 0; ch < m_waveFormat->nChannels; ++ch)
                    sum += s[i * m_waveFormat->nChannels + ch];
                mono[i] = (float)sum / (32768.0f * m_waveFormat->nChannels);
            }
        }

        m_captureClient->ReleaseBuffer(frames);
        ProcessAudioBuffer(mono);
    }
}

void AudioVisualizerRenderer::ProcessAudioBuffer(const std::vector<float>& samples) {
    std::lock_guard lock(m_audioMutex);

    m_waveform.insert(m_waveform.end(), samples.begin(), samples.end());
    if (m_waveform.size() > 4096)
        m_waveform.erase(m_waveform.begin(), m_waveform.begin() + (m_waveform.size() - 2048));

    if (m_waveform.size() >= (size_t)FFT_SIZE) {
        std::vector<float> re(FFT_SIZE);
        std::vector<float> im(FFT_SIZE, 0.0f);

        // Apply Hann window
        auto wavEnd = m_waveform.end();
        for (int i = 0; i < FFT_SIZE; ++i) {
            float hann = 0.5f * (1.0f - cosf(6.2831853f * i / (FFT_SIZE - 1)));
            re[i] = *(wavEnd - FFT_SIZE + i) * hann;
        }

        Fft(re, im);

        m_spectrum.resize(m_numBars);
        int samplesPerBar = (FFT_SIZE / 2) / m_numBars;
        for (UINT b = 0; b < m_numBars; ++b) {
            float sum = 0;
            for (int j = 0; j < samplesPerBar; ++j) {
                int idx = b * samplesPerBar + j;
                if (idx < FFT_SIZE / 2) {
                    float mag = sqrtf(re[idx] * re[idx] + im[idx] * im[idx]);
                    sum += mag;
                }
            }
            m_spectrum[b] = (sum / samplesPerBar) * 2.0f;
            if (m_spectrum[b] > 1.0f) m_spectrum[b] = 1.0f;
        }

        m_newFrame = true;
    }
}

// ─── Rendering ────────────────────────────────────────────────────────

struct CbLayout {
    float time, resX, resY, mode;
    float numBars, pad0, pad1, pad2;
    float bars[32];
};

void AudioVisualizerRenderer::DrawBars() {
    if (!m_rootSig || !m_psoBars) return;

    std::vector<float> spectrum;
    {
        std::lock_guard lock(m_audioMutex);
        spectrum = m_spectrum;
        m_newFrame = false;
    }

    // Fill CB
    CbLayout* cb = reinterpret_cast<CbLayout*>(m_mappedSpectrum);
    cb->time = m_time;
    cb->resX = (float)m_width;
    cb->resY = (float)m_height;
    cb->mode = 0.0f;
    cb->numBars = (float)m_numBars;
    cb->pad0 = cb->pad1 = cb->pad2 = 0;
    for (UINT i = 0; i < m_numBars; ++i)
        cb->bars[i] = i < spectrum.size() ? spectrum[i] : 0;

    m_commandAllocator->Reset();
    m_commandList->Reset(m_commandAllocator.Get(), nullptr);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_sharedTexture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    m_commandList->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    const float clear[] = {0,0,0,1};
    m_commandList->ClearRenderTargetView(rtv, clear, 0, nullptr);

    m_commandList->SetPipelineState(m_psoBars.Get());
    m_commandList->SetGraphicsRootSignature(m_rootSig.Get());
    m_commandList->SetGraphicsRootConstantBufferView(0, m_uploadBuffer->GetGPUVirtualAddress());
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_commandList->DrawInstanced(3, 1, 0, 0);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    m_commandList->ResourceBarrier(1, &barrier);
    m_commandList->Close();

    ID3D12CommandList* lists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);
}

void AudioVisualizerRenderer::DrawWaveform() {
    if (!m_rootSig || !m_psoWaveform) return;

    std::vector<float> spectrum;
    {
        std::lock_guard lock(m_audioMutex);
        spectrum = m_spectrum;
        m_newFrame = false;
    }

    CbLayout* cb = reinterpret_cast<CbLayout*>(m_mappedSpectrum);
    cb->time = m_time;
    cb->resX = (float)m_width;
    cb->resY = (float)m_height;
    cb->mode = 1.0f;
    cb->numBars = (float)m_numBars;
    cb->pad0 = cb->pad1 = cb->pad2 = 0;
    for (UINT i = 0; i < m_numBars; ++i)
        cb->bars[i] = i < spectrum.size() ? spectrum[i] : 0;

    m_commandAllocator->Reset();
    m_commandList->Reset(m_commandAllocator.Get(), nullptr);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_sharedTexture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    m_commandList->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    const float clear[] = {0,0,0,1};
    m_commandList->ClearRenderTargetView(rtv, clear, 0, nullptr);

    m_commandList->SetPipelineState(m_psoWaveform.Get());
    m_commandList->SetGraphicsRootSignature(m_rootSig.Get());
    m_commandList->SetGraphicsRootConstantBufferView(0, m_uploadBuffer->GetGPUVirtualAddress());
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_commandList->DrawInstanced(3, 1, 0, 0);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    m_commandList->ResourceBarrier(1, &barrier);
    m_commandList->Close();

    ID3D12CommandList* lists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);
}

void AudioVisualizerRenderer::DrawParticles() {
    if (!m_rootSig || !m_psoParticles) return;

    std::vector<float> spectrum;
    {
        std::lock_guard lock(m_audioMutex);
        spectrum = m_spectrum;
        m_newFrame = false;
    }

    CbLayout* cb = reinterpret_cast<CbLayout*>(m_mappedSpectrum);
    cb->time = m_time;
    cb->resX = (float)m_width;
    cb->resY = (float)m_height;
    cb->mode = 2.0f;
    cb->numBars = (float)m_numBars;
    cb->pad0 = cb->pad1 = cb->pad2 = 0;
    for (UINT i = 0; i < m_numBars; ++i)
        cb->bars[i] = i < spectrum.size() ? spectrum[i] : 0;

    m_commandAllocator->Reset();
    m_commandList->Reset(m_commandAllocator.Get(), nullptr);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_sharedTexture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    m_commandList->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    const float clear[] = {0,0,0,1};
    m_commandList->ClearRenderTargetView(rtv, clear, 0, nullptr);

    m_commandList->SetPipelineState(m_psoParticles.Get());
    m_commandList->SetGraphicsRootSignature(m_rootSig.Get());
    m_commandList->SetGraphicsRootConstantBufferView(0, m_uploadBuffer->GetGPUVirtualAddress());
    m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_commandList->DrawInstanced(3, 1, 0, 0);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    m_commandList->ResourceBarrier(1, &barrier);
    m_commandList->Close();

    ID3D12CommandList* lists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);
}

} // namespace rex::wallpaper

``

## src/wallpaper/renderer/AudioVisualizerRenderer.hpp

``cpp
#pragma once
#include "IRenderer.hpp"
#include <d3d12.h>
#include <wrl/client.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <memory>
#include <thread>
#include <string>

class Compositor;

namespace rex::wallpaper {

class AudioVisualizerRenderer : public IRenderer {
public:
    AudioVisualizerRenderer();
    ~AudioVisualizerRenderer() override;

    bool load(const SceneInfo& scene, Compositor& compositor) override;
    void update(double deltaTime) override;
    void render(Compositor& compositor) override;
    void unload() override;

    void play() override { m_playing.store(true); }
    void pause() override { m_playing.store(false); }
    void stop() override { m_playing.store(false); }
    bool isPlaying() const override { return m_playing.load(); }

private:
    bool InitD3D12(Compositor& compositor, int width, int height);
    bool InitAudioCapture();
    void AudioCaptureThread();
    void ProcessAudioBuffer(const std::vector<float>& samples);
    void DrawBars();
    void DrawWaveform();
    void DrawParticles();
    void CompileShaders();

    enum class VisualType { Bars, Waveform, Particles };
    VisualType m_visualType = VisualType::Bars;

    // D3D12
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_commandQueue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_commandAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commandList;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_sharedTexture;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_sharedFence;
    HANDLE m_sharedHandle = nullptr;
    HANDLE m_fenceHandle = nullptr;
    UINT64 m_fenceValue = 1;

    // Pipeline for full-screen triangle
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoBars;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoWaveform;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_psoParticles;

    // Spectrum upload buffer (mapped)
    Microsoft::WRL::ComPtr<ID3D12Resource> m_uploadBuffer;
    float* m_mappedSpectrum = nullptr;
    UINT m_numBars = 32;

    // Audio
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> m_enum;
    Microsoft::WRL::ComPtr<IMMDevice> m_audioDevice;
    Microsoft::WRL::ComPtr<IAudioClient> m_audioClient;
    Microsoft::WRL::ComPtr<IAudioCaptureClient> m_captureClient;
    WAVEFORMATEX* m_waveFormat = nullptr;
    std::atomic<bool> m_audioRunning{false};
    std::thread m_audioThread;

    // Buffers
    std::vector<float> m_audioBuffer;
    std::vector<float> m_spectrum;
    std::vector<float> m_waveform;
    std::atomic<bool> m_newFrame{false};
    std::mutex m_audioMutex;

    int m_width = 0, m_height = 0;
    std::atomic<bool> m_playing{false};
    float m_time = 0.0f;
    Compositor* m_compositor = nullptr;
    static constexpr uint64_t AV_LAYER_ID = 4;
    static constexpr int FFT_SIZE = 1024;
};

} // namespace

``

## src/wallpaper/renderer/RendererManager.hpp

``cpp
#pragma once
#include "IRenderer.hpp"
#include <memory>

namespace rex::wallpaper {

class RendererManager {
public:
    void setRenderer(std::unique_ptr<IRenderer> renderer);
    IRenderer* getRenderer() { return m_renderer.get(); }
    const IRenderer* getRenderer() const { return m_renderer.get(); }
    void clear();

    void update(double dt) { if (m_renderer) m_renderer->update(dt); }
    void render(::Compositor& comp) { if (m_renderer) m_renderer->render(comp); }
    void play() { if (m_renderer) m_renderer->play(); }
    void pause() { if (m_renderer) m_renderer->pause(); }
    void stop() { if (m_renderer) m_renderer->stop(); }

private:
    std::unique_ptr<IRenderer> m_renderer;
};

inline void RendererManager::setRenderer(std::unique_ptr<IRenderer> renderer) {
    if (m_renderer) m_renderer->unload();
    m_renderer = std::move(renderer);
}

inline void RendererManager::clear() {
    if (m_renderer) m_renderer->unload();
    m_renderer.reset();
}

} // namespace

``

## src/wallpaper/renderer/IRenderer.hpp

``cpp
#pragma once

#include <rex/wallpaper/IWallpaperEngine.hpp>

class Compositor;

namespace rex::wallpaper {

class IRenderer {
public:
    virtual ~IRenderer() = default;

    virtual bool load(const SceneInfo& scene, Compositor& compositor) = 0;
    virtual void update(double deltaTime) = 0;
    virtual void render(Compositor& compositor) = 0;
    virtual void unload() = 0;

    virtual void play() {}
    virtual void pause() {}
    virtual void stop() {}
    virtual bool isPlaying() const { return false; }
    virtual double getDuration() const { return 0.0; }
    virtual double getWidth() const { return 0.0; }
    virtual double getHeight() const { return 0.0; }
};

} // namespace rex::wallpaper

``

## src/wallpaper/renderer/ECS.hpp

``cpp
#pragma once
#include <vector>
#include <memory>
#include <DirectXMath.h>

namespace rex::wallpaper {

using namespace DirectX;

struct Transform {
    XMFLOAT3 position = {0,0,0};
    XMFLOAT3 rotation = {0,0,0};
    XMFLOAT3 scale    = {1,1,1};
    XMMATRIX GetMatrix() const {
        return XMMatrixScaling(scale.x, scale.y, scale.z) *
               XMMatrixRotationRollPitchYaw(rotation.x, rotation.y, rotation.z) *
               XMMatrixTranslation(position.x, position.y, position.z);
    }
};

struct MeshComponent {
    uint32_t vertexBufferId;
    uint32_t indexBufferId;
    uint32_t indexCount;
    uint32_t materialId;
};

struct CameraComponent {
    float fov      = XM_PIDIV4;
    float nearPlane = 0.1f;
    float farPlane  = 1000.0f;
    bool active     = false;
};

struct LightComponent {
    XMFLOAT3 color     = {1,1,1};
    float    intensity = 1.0f;
    bool     castShadows = false;
};

struct Entity {
    uint32_t id;
    Transform transform;
    std::unique_ptr<MeshComponent> mesh;
    std::unique_ptr<CameraComponent> camera;
    std::unique_ptr<LightComponent> light;
};

class ECS {
public:
    Entity* CreateEntity() {
        m_entities.emplace_back();
        m_entities.back().id = m_nextId++;
        return &m_entities.back();
    }

    void RemoveEntity(uint32_t id) {
        std::erase_if(m_entities, [id](const Entity& e) { return e.id == id; });
    }

    std::vector<Entity>& GetEntities() { return m_entities; }
    const std::vector<Entity>& GetEntities() const { return m_entities; }

    Entity* FindEntity(uint32_t id) {
        for (auto& e : m_entities)
            if (e.id == id) return &e;
        return nullptr;
    }

    CameraComponent* GetActiveCamera() {
        for (auto& e : m_entities)
            if (e.camera && e.camera->active) return e.camera.get();
        return nullptr;
    }

private:
    std::vector<Entity> m_entities;
    uint32_t m_nextId = 1;
};

} // namespace

``

## src/wallpaper/monitor/MonitorCompositor.cpp

``cpp
#include "wallpaper/monitor/MonitorCompositor.hpp"

#include <shared/Logger.hpp>

namespace rex::wallpaper {

ATOM MonitorCompositor::s_windowClass = 0;

namespace {
constexpr const char* kModule = "MonitorCompositor";
constexpr wchar_t kWindowClassName[] = L"REXMonitorWindow";

LRESULT CALLBACK MonitorWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
} // namespace

MonitorCompositor::MonitorCompositor(MonitorInfo info)
    : m_info(std::move(info)) {
}

MonitorCompositor::~MonitorCompositor() {
    Shutdown();
}

ATOM MonitorCompositor::RegisterWindowClass() {
    if (s_windowClass) return s_windowClass;

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = MonitorWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = kWindowClassName;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    s_windowClass    = RegisterClassExW(&wc);
    return s_windowClass;
}

bool MonitorCompositor::Initialize(ID3D12Device* device, HWND workerW) {
    if (m_active) return true;
    if (!device || !workerW || !IsWindow(workerW)) {
        REX_LOG_ERROR(kModule, "[{}] invalid device/parent for init",
                      m_info.id.c_str());
        return false;
    }

    if (!RegisterWindowClass()) {
        REX_LOG_ERROR(kModule, "[{}] Failed to register monitor window class (err={})",
                      m_info.id.c_str(), static_cast<long>(GetLastError()));
        return false;
    }

    // Child of the wallpaper WorkerW: sits in the wallpaper layer, below
    // the desktop icon layer, above the engine's main render window.
    m_window = CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        kWindowClassName, L"",
        WS_CHILD | WS_VISIBLE,
        m_info.x, m_info.y, m_info.width, m_info.height,
        workerW, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!m_window) {
        REX_LOG_ERROR(kModule, "[{}] CreateWindowEx failed (err={})",
                      m_info.id.c_str(), static_cast<long>(GetLastError()));
        return false;
    }

    // Above the main render window (sibling in the same WorkerW), so this
    // monitor's content is never occluded by the main window.
    SetWindowPos(m_window, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    m_renderer = std::make_unique<D3D12Renderer>();
    auto result = m_renderer->Initialize(m_window, m_info.width,
                                         m_info.height, device);
    if (result.IsError()) {
        REX_LOG_ERROR(kModule, "[{}] renderer init failed: {}",
                      m_info.id.c_str(), result.Error().message);
        DestroyWindow(m_window);
        m_window = nullptr;
        m_renderer.reset();
        return false;
    }

    m_active = true;
    REX_LOG_INFO(kModule, "[{}] initialized {}x{} at ({},{})",
                 m_info.id.c_str(), m_info.width, m_info.height,
                 m_info.x, m_info.y);
    return true;
}

void MonitorCompositor::Shutdown() {
    if (m_renderer) {
        m_renderer->Shutdown();
        m_renderer.reset();
    }
    if (m_window) {
        DestroyWindow(m_window);
        m_window = nullptr;
    }
    m_active = false;
}

void MonitorCompositor::PresentFrame(const VideoFrame& frame,
                                     ID3D12Fence* waitFence) {
    if (!m_active || !m_renderer || !frame.valid) return;

    m_renderer->BeginFrame();                    // 1. prepare
    m_renderer->DrawVideoFrame(frame, waitFence); // 2. bind shared slot + fence
    m_renderer->RenderFrame();                   // 3. sample + draw
    m_renderer->EndFrame();                      // 4. present
    m_frameCount.fetch_add(1, std::memory_order_relaxed);
}

void MonitorCompositor::PresentStaticTexture(const std::string& path) {
    if (!m_active || !m_renderer) return;

    auto result = m_renderer->LoadTexture(path);
    if (result.IsError()) {
        REX_LOG_WARN(kModule, "[{}] texture load failed: {}",
                     m_info.id.c_str(), result.Error().message);
        return;
    }
    // Static content has no per-frame updates: draw once; the swap chain
    // keeps presenting the last frame.
    m_renderer->BeginFrame();
    m_renderer->RenderFrame();
    m_renderer->EndFrame();
    m_frameCount.fetch_add(1, std::memory_order_relaxed);
}

void MonitorCompositor::ClearStaticTexture() {
    if (m_renderer) {
        m_renderer->ClearTexture();
    }
}

void MonitorCompositor::SetVideoAdjustments(float brightness, float contrast,
                                            float saturation) {
    if (m_renderer) {
        m_renderer->SetVideoAdjustments(brightness, contrast, saturation);
    }
}

void MonitorCompositor::ClearVideoFrame() {
    if (m_renderer) {
        m_renderer->ClearVideoFrame();
    }
}

void MonitorCompositor::SetFillMode(FillMode mode) {
    if (m_renderer) {
        m_renderer->SetFillMode(mode);
    }
}

void MonitorCompositor::SetTransform(float rotationDegrees, bool flipH,
                                     bool flipV) {
    if (m_renderer) {
        m_renderer->SetRotation(rotationDegrees);
        m_renderer->SetFlip(flipH, flipV);
    }
}

void MonitorCompositor::Resize(int width, int height) {
    if (!m_active || !m_renderer) return;

    m_info.width  = width;
    m_info.height = height;
    if (m_window) {
        MoveWindow(m_window, m_info.x, m_info.y, width, height, TRUE);
    }
    m_renderer->Resize(width, height);
}

void MonitorCompositor::SetPosition(int x, int y) {
    if (!m_active || !m_window) return;

    m_info.x = x;
    m_info.y = y;
    MoveWindow(m_window, x, y, m_info.width, m_info.height, TRUE);
}

} // namespace rex::wallpaper
``

## src/wallpaper/monitor/MonitorCompositor.hpp

``cpp
#pragma once

#include "graphics/renderer/D3D12Renderer.hpp"

#include <windows.h>
#include <rex/wallpaper/IWallpaperEngine.hpp>

#include <atomic>
#include <memory>
#include <string>

namespace rex::wallpaper {

// ─── MonitorCompositor ─────────────────────────────────────────────────────
//
// Per-monitor presentation context. Owns a child render window inside the
// wallpaper WorkerW layer (a sibling of the engine's main render window,
// positioned exactly on this monitor's rect) plus a D3D12Renderer that
// ADOPTS the engine's D3D12 device — the same device that created the video
// pool slots' NT handles, so shared-handle frames stay valid on any GPU
// topology.
//
// Window hierarchy (siblings under the wallpaper WorkerW, all below the
// desktop icon layer):
//
//   WorkerW (wallpaper layer)
//   ├── REXRenderWindow      (main, full virtual desktop — engine renderer)
//   └── REXMonitorWindow     (one per monitor, HWND_TOP of the main window)
//
// Compositors sit ABOVE the main render window so their content is not
// occluded on their monitor; the main window only matters for monitors that
// have no compositor (never in practice — one is created per monitor).
//
// Threading: all Present* calls must come from the render thread
// (Engine::RenderLoop) — same convention as the main renderer.

class MonitorCompositor {
public:
    explicit MonitorCompositor(MonitorInfo info);
    ~MonitorCompositor();

    MonitorCompositor(const MonitorCompositor&) = delete;
    MonitorCompositor& operator=(const MonitorCompositor&) = delete;

    /// workerW: the wallpaper WorkerW (parent of the engine's render
    /// window). device: the engine's D3D12 device (adopted by the renderer).
    bool Initialize(ID3D12Device* device, HWND workerW);
    void Shutdown();

    /// Present the current video frame (a decoder pool slot) to this
    /// monitor. The command queue waits on waitFence/fenceValue before
    /// sampling (same contract as D3D12Renderer::DrawVideoFrame). Takes
    /// the real VideoFrame so HDR colorimetry travels with the frame.
    /// Order inside: BeginFrame -> DrawVideoFrame -> RenderFrame -> EndFrame.
    void PresentFrame(const VideoFrame& frame, ID3D12Fence* waitFence);

    /// Load and present a static image (per-monitor copy; the main
    /// renderer's texture is not shareable across devices).
    void PresentStaticTexture(const std::string& path);
    void ClearStaticTexture();

    /// Video adjustments (identity 1.0). Fan-out from the engine so
    /// per-monitor renderers match the main renderer.
    void SetVideoAdjustments(float brightness, float contrast, float saturation);

    /// Rotation/flip transform. Fan-out from the engine so per-monitor
    /// renderers match the main renderer.
    void SetTransform(float rotationDegrees, bool flipH, bool flipV);

    /// Drop GPU video state (scene unload). MUST NOT be called between
    /// BeginFrame()/EndFrame().
    void ClearVideoFrame();

    void SetFillMode(FillMode mode);
    void Resize(int width, int height);
    void SetPosition(int x, int y);

    const MonitorInfo& GetInfo() const { return m_info; }
    bool IsActive() const { return m_active; }
    bool IsDeviceRemoved() const {
        return m_renderer && m_renderer->IsDeviceRemoved();
    }
    uint64_t GetFrameCount() const { return m_frameCount; }

private:
    static ATOM RegisterWindowClass();

    void PresentNow();

    MonitorInfo m_info;
    bool m_active = false;
    HWND m_window = nullptr;
    std::unique_ptr<D3D12Renderer> m_renderer;
    std::atomic<uint64_t> m_frameCount{0};
    static ATOM s_windowClass;
};

} // namespace rex::wallpaper

``

## src/wallpaper/scene/StaticImageScene.cpp

``cpp
#include "StaticImageScene.hpp"

#include <shared/Logger.hpp>

namespace rex::wallpaper::scene {

// ─── Construction / Destruction ─────────────────────────────────────────────

StaticImageScene::StaticImageScene(
    std::shared_ptr<loader::ImageLoader> imageLoader)
    : m_imageLoader(std::move(imageLoader))
{
}

StaticImageScene::~StaticImageScene() {
    Unload();
}

// ─── Load ───────────────────────────────────────────────────────────────────

shared::Result<void> StaticImageScene::Load(std::string_view path) {
    if (path.empty()) {
        return shared::Result<void>(shared::ErrorCode::InvalidArgument,
            "Empty image path");
    }

    REX_LOG_INFO("StaticImageScene", "Loading: {}", path);

    auto result = m_imageLoader->Load(path);
    if (result.IsError()) {
        return shared::Result<void>(
            result.Error().code, result.Error().message);
    }

    m_texture = result.Value();
    m_width   = m_texture->pixels.width;
    m_height  = m_texture->pixels.height;
    m_version = m_texture->version;
    m_path    = path;
    m_loaded  = true;

    REX_LOG_INFO("StaticImageScene",
        "Loaded {}x{} image (v{})", m_width, m_height, m_version);

    return shared::Result<void>();
}

// ─── Reload ─────────────────────────────────────────────────────────────────

shared::Result<void> StaticImageScene::Reload() {
    if (m_path.empty()) {
        return shared::Result<void>(shared::ErrorCode::NotInitialized,
            "No image loaded");
    }

    auto result = m_imageLoader->Reload(m_path);
    if (result.IsError()) {
        return shared::Result<void>(
            result.Error().code, result.Error().message);
    }

    m_texture = result.Value();
    m_width   = m_texture->pixels.width;
    m_height  = m_texture->pixels.height;
    m_version = m_texture->version;

    REX_LOG_INFO("StaticImageScene",
        "Reloaded {}x{} (v{})", m_width, m_height, m_version);

    return shared::Result<void>();
}

// ─── Unload ─────────────────────────────────────────────────────────────────

void StaticImageScene::Unload() {
    m_loaded  = false;
    m_texture = nullptr;
    m_width   = 0;
    m_height  = 0;
    m_path.clear();
}

// ─── Pixel data access ─────────────────────────────────────────────────────

const uint8_t* StaticImageScene::GetPixels() const {
    if (!m_texture || !m_loaded) return nullptr;
    return m_texture->pixels.pixels.data();
}

int StaticImageScene::GetStride() const {
    if (!m_texture || !m_loaded) return 0;
    // RGBA8 = 4 bytes per pixel
    return 4;
}

int StaticImageScene::GetRowPitch() const {
    if (!m_texture || !m_loaded) return 0;
    return m_texture->pixels.rowPitch;
}

} // namespace rex::wallpaper::scene

``

## src/wallpaper/scene/StaticImageScene.hpp

``cpp
#pragma once

#include "graphics/resource/TextureCache.hpp"
#include "wallpaper/loader/ImageLoader.hpp"

#include <rex/wallpaper/IWallpaperEngine.hpp>

#include <shared/Result.hpp>

#include <memory>
#include <string>
#include <string_view>

namespace rex::wallpaper::scene {

// ─── StaticImageScene ───────────────────────────────────────────────────────
//
// Manages a single static image wallpaper.
//
// Owns the scene lifecycle:
//   1. Load(imagePath)  — decode image, upload to GPU
//   2. ApplyTo(renderer) — configure the renderer's texture + fill mode
//   3. Unload()         — release GPU texture, evict from cache
//
// ## Fill modes
//
//   Fill    — scale to cover viewport, crop excess (default)
//   Fit     — scale to fit within viewport, letterbox
//   Stretch — stretch to fill viewport, ignore aspect ratio
//   Center  — center at native resolution, no scaling
//
// ## Hot-reload
//
// When the image file is modified on disk, call Reload() to re-decode
// and re-upload to GPU. The renderer will pick up the new texture on
// the next frame.
//
// ## Thread safety
//
// Load/Unload/Reload must be called from the engine's render thread.
// The underlying ImageLoader is thread-safe for concurrent loads.

class StaticImageScene {
public:
    StaticImageScene(std::shared_ptr<loader::ImageLoader> imageLoader);
    ~StaticImageScene();

    StaticImageScene(const StaticImageScene&) = delete;
    StaticImageScene& operator=(const StaticImageScene&) = delete;

    // ── Lifecycle ──────────────────────────────────────────────────────

    /// Load the image at `path`. Decodes via WIC and caches pixel data.
    shared::Result<void> Load(std::string_view path);

    /// Reload from disk (hot-reload).
    shared::Result<void> Reload();

    /// Release GPU resources.
    void Unload();

    // ── State ──────────────────────────────────────────────────────────

    bool IsLoaded() const { return m_loaded; }
    std::string_view GetPath() const { return m_path; }
    int GetImageWidth()  const { return m_width; }
    int GetImageHeight() const { return m_height; }
    FillMode GetFillMode() const { return m_fillMode; }

    // ── Settings ───────────────────────────────────────────────────────

    void SetFillMode(FillMode mode) { m_fillMode = mode; }
    void SetBrightness(float value) { m_brightness = value; }
    void SetContrast(float value)   { m_contrast = value; }
    void SetSaturation(float value) { m_saturation = value; }

    float GetBrightness() const { return m_brightness; }
    float GetContrast()   const { return m_contrast; }
    float GetSaturation() const { return m_saturation; }

    // ── Pixel data ─────────────────────────────────────────────────────

    /// Raw pixel data for GPU upload. Valid after Load().
    const uint8_t* GetPixels()   const;
    int            GetStride()   const;
    int            GetRowPitch() const;

    /// Version counter, incremented on each reload.
    /// The renderer can track this to detect when re-upload is needed.
    uint64_t GetVersion() const { return m_version; }

private:
    std::shared_ptr<loader::ImageLoader> m_imageLoader;
    std::string m_path;
    bool m_loaded = false;

    int m_width  = 0;
    int m_height = 0;

    FillMode m_fillMode    = FillMode::Fill;
    float m_brightness     = 1.0f;
    float m_contrast       = 1.0f;
    float m_saturation     = 1.0f;

    // Pixel cache pointer (owned by TextureCache)
    const graphics::CachedTexture* m_texture = nullptr;
    uint64_t m_version = 0;
};

} // namespace rex::wallpaper::scene

``

## src/wallpaper/runtime/RuntimeSettings.cpp

``cpp
#include "RuntimeSettings.hpp"
#include <fstream>
#include <sstream>
#include <shared/Logger.hpp>

namespace rex::wallpaper {

RuntimeSettings::RuntimeSettings(const std::filesystem::path& file)
    : m_file(file) {
    Load();
}

bool RuntimeSettings::Load() {
    std::lock_guard lock(m_mutex);
    m_data.clear();
    if (!std::filesystem::exists(m_file)) return false;
    std::ifstream in(m_file);
    if (!in.is_open()) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    return ParseJson(ss.str());
}

bool RuntimeSettings::Save() {
    std::lock_guard lock(m_mutex);
    const std::string json = SerializeJson();
    const std::filesystem::path tmp = m_file;
    const std::string tmpPath = tmp.string() + ".tmp";
    std::ofstream out(tmpPath, std::ios::trunc);
    if (!out.is_open()) return false;
    out << json;
    out.close();
    std::error_code ec;
    std::filesystem::rename(tmpPath, m_file, ec);
    return !ec;
}

void RuntimeSettings::SetMonitorWallpaper(uint64_t edidHash, const std::string& path) {
    SetString("monitor." + std::to_string(edidHash) + ".wallpaper", path);
}

std::string RuntimeSettings::GetMonitorWallpaper(uint64_t edidHash) const {
    return GetString("monitor." + std::to_string(edidHash) + ".wallpaper");
}

void RuntimeSettings::RemoveMonitorWallpaper(uint64_t edidHash) {
    std::lock_guard lock(m_mutex);
    m_data.erase("monitor." + std::to_string(edidHash) + ".wallpaper");
    Save();
}

void RuntimeSettings::SetString(const std::string& key, const std::string& value) {
    std::lock_guard lock(m_mutex);
    m_data[key] = value;
    Save();
}

std::string RuntimeSettings::GetString(const std::string& key, const std::string& def) const {
    std::lock_guard lock(m_mutex);
    auto it = m_data.find(key);
    return it != m_data.end() ? it->second : def;
}

void RuntimeSettings::RemoveKey(const std::string& key) {
    std::lock_guard lock(m_mutex);
    m_data.erase(key);
    Save();
}

std::vector<std::string> RuntimeSettings::GetKeysByPrefix(const std::string& prefix) const {
    std::lock_guard lock(m_mutex);
    std::vector<std::string> result;
    for (const auto& [k, v] : m_data) {
        if (k.rfind(prefix, 0) == 0) {
            result.push_back(k);
        }
    }
    return result;
}

// ── Minimal JSON parser/serializer ────────────────────────────────────────
// Flat {"key":"value"} object only — sufficient for a string→string map.

std::string RuntimeSettings::EscapeJson(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

std::string RuntimeSettings::UnescapeJson(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            ++i;
            switch (s[i]) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                default:   out += s[i]; break;
            }
        } else {
            out += s[i];
        }
    }
    return out;
}

bool RuntimeSettings::ParseJson(const std::string& text) {
    m_data.clear();
    size_t pos = text.find('{');
    if (pos == std::string::npos) return false;
    pos++;
    while (pos < text.size()) {
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\n' ||
               text[pos] == '\r' || text[pos] == '\t')) pos++;
        if (pos >= text.size()) break;
        if (text[pos] == '}') break;
        if (text[pos] != '"') return false;
        pos++;
        std::string key;
        while (pos < text.size() && text[pos] != '"') {
            if (text[pos] == '\\' && pos + 1 < text.size()) {
                pos++;
                switch (text[pos]) {
                    case '"':  key += '"';  break;
                    case '\\': key += '\\'; break;
                    case 'n':  key += '\n'; break;
                    case 't':  key += '\t'; break;
                    default:   key += text[pos]; break;
                }
                pos++;
            } else {
                key += text[pos++];
            }
        }
        if (pos >= text.size()) return false;
        pos++; // closing quote
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\n')) pos++;
        if (pos >= text.size() || text[pos] != ':') return false;
        pos++;
        while (pos < text.size() && text[pos] == ' ') pos++;
        if (pos >= text.size() || text[pos] != '"') return false;
        pos++;
        std::string value;
        while (pos < text.size() && text[pos] != '"') {
            if (text[pos] == '\\' && pos + 1 < text.size()) {
                pos++;
                switch (text[pos]) {
                    case '"':  value += '"';  break;
                    case '\\': value += '\\'; break;
                    case 'n':  value += '\n'; break;
                    case 't':  value += '\t'; break;
                    default:   value += text[pos]; break;
                }
                pos++;
            } else {
                value += text[pos++];
            }
        }
        if (pos >= text.size()) return false;
        pos++; // closing quote
        m_data[key] = value;
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == ',' ||
               text[pos] == '\n' || text[pos] == '\r')) pos++;
    }
    return true;
}

std::string RuntimeSettings::SerializeJson() const {
    std::string out = "{";
    bool first = true;
    for (const auto& [k, v] : m_data) {
        if (!first) out += ",";
        first = false;
        out += "\"" + EscapeJson(k) + "\":\"" + EscapeJson(v) + "\"";
    }
    out += "}";
    return out;
}

} // namespace rex::wallpaper

``

## src/wallpaper/runtime/RuntimeSettings.hpp

``cpp
#pragma once
#include <string>
#include <unordered_map>
#include <mutex>
#include <filesystem>

namespace rex::wallpaper {

// Lightweight persistent JSON settings for the runtime (no full Settings
// service). Stores per-monitor wallpaper assignments and playlist data as a
// flat string→string map, persisted to a small JSON file. Replaces the heavy
// Settings Framework (registry + validation + search + history + backup +
// schema + import/export) which added 20–30 MB.
class RuntimeSettings {
public:
    explicit RuntimeSettings(const std::filesystem::path& file);
    ~RuntimeSettings() = default;

    RuntimeSettings(const RuntimeSettings&) = delete;
    RuntimeSettings& operator=(const RuntimeSettings&) = delete;

    bool Load();
    bool Save();

    // Per-monitor wallpaper (EDID-keyed)
    void SetMonitorWallpaper(uint64_t edidHash, const std::string& path);
    std::string GetMonitorWallpaper(uint64_t edidHash) const;
    void RemoveMonitorWallpaper(uint64_t edidHash);

    // Playlist persistence
    void SetString(const std::string& key, const std::string& value);
    std::string GetString(const std::string& key, const std::string& def = "") const;
    void RemoveKey(const std::string& key);

    // Return all keys matching a prefix (for garbage-collecting stale entries).
    std::vector<std::string> GetKeysByPrefix(const std::string& prefix) const;

private:
    std::filesystem::path m_file;
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, std::string> m_data;

    static std::string EscapeJson(const std::string& s);
    static std::string UnescapeJson(const std::string& s);
    bool ParseJson(const std::string& text);
    std::string SerializeJson() const;
};

} // namespace rex::wallpaper

``

## src/wallpaper/runtime/RendererAdapters.cpp

``cpp
#include <rex/wallpaper/IRendererAdapter.hpp>

#include <set>

namespace rex::wallpaper {

// ─── FillMode helpers ──────────────────────────────────────────────────────

std::optional<FillMode> ParseFillMode(std::string_view s) {
    if (s == "fill")    return FillMode::Fill;
    if (s == "fit")     return FillMode::Fit;
    if (s == "stretch") return FillMode::Stretch;
    if (s == "center")  return FillMode::Center;
    if (s == "tile")    return FillMode::Tile;
    return std::nullopt;
}

std::string_view ToString(FillMode mode) {
    switch (mode) {
        case FillMode::Fill:    return "fill";
        case FillMode::Fit:     return "fit";
        case FillMode::Stretch: return "stretch";
        case FillMode::Center:  return "center";
        case FillMode::Tile:    return "tile";
    }
    return "fill";
}

namespace {

class BaseAdapter : public IRendererAdapter {
public:
    std::vector<std::string> GetSupportedProperties() const override {
        std::vector<std::string> props;
        props.reserve(m_supported.size());
        for (const auto& p : m_supported) {
            props.emplace_back(p);
        }
        return props;
    }

    bool Supports(std::string_view property) const override {
        return m_supported.find(property) != m_supported.end();
    }

protected:
    std::set<std::string, std::less<>> m_supported;
};

// Static image: rendered via a GPU texture. Only the fill mode is currently
// applied; image effects (brightness, contrast, ...) have no D3D12 pipeline
// yet, so they are reported as unsupported rather than silently ignored.
class StaticImageAdapter : public BaseAdapter {
public:
    std::string_view GetType() const override { return "static"; }
    StaticImageAdapter() {
        m_supported.insert("fillMode");
        m_supported.insert("rotation");
        m_supported.insert("flipH");
        m_supported.insert("flipV");
    }
};

// Video: fill mode via the renderer, speed/loop via the playback controller.
// Volume/mute have no audio pipeline yet — reported unsupported.
class VideoAdapter : public BaseAdapter {
public:
    std::string_view GetType() const override { return "video"; }
    VideoAdapter() {
        m_supported.insert("fillMode");
        m_supported.insert("speed");
        m_supported.insert("loop");
        m_supported.insert("rotation");
        m_supported.insert("flipH");
        m_supported.insert("flipV");
    }
};

// Web / shader / scene / audio-visualizer renderers are not implemented in
// the engine yet. They accept no properties — every apply is rejected.
class UnsupportedAdapter : public BaseAdapter {
public:
    std::string_view GetType() const override { return "unsupported"; }
};

} // namespace

std::unique_ptr<IRendererAdapter> CreateRendererAdapter(std::string_view sceneType) {
    if (sceneType == "video") return std::make_unique<VideoAdapter>();
    if (sceneType == "static" || sceneType == "image") return std::make_unique<StaticImageAdapter>();
    return std::make_unique<UnsupportedAdapter>();
}

} // namespace rex::wallpaper

``

## src/wallpaper/runtime/Json.hpp

``cpp
#pragma once

// ─── Json ──────────────────────────────────────────────────────────────────
// Minimal dependency-free JSON value, parser, and serializer used by the
// wallpaper runtime command protocol. Not a general-purpose JSON library:
// it supports standard JSON objects, arrays, strings, numbers, booleans,
// and null, with a nesting depth limit for safety.

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace rex::runtime {

class Json {
public:
    using Object = std::map<std::string, Json, std::less<>>;
    using Array  = std::vector<Json>;
    using Value  = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;

    Json() : m_value(nullptr) {}
    Json(std::nullptr_t) : m_value(nullptr) {}
    Json(bool b) : m_value(b) {}
    Json(int v) : m_value(static_cast<double>(v)) {}
    Json(unsigned v) : m_value(static_cast<double>(v)) {}
    Json(long long v) : m_value(static_cast<double>(v)) {}
    Json(double v) : m_value(v) {}
    Json(float v) : m_value(static_cast<double>(v)) {}
    Json(const char* s) : m_value(std::string(s)) {}
    Json(std::string s) : m_value(std::move(s)) {}
    Json(Array a) : m_value(std::move(a)) {}
    Json(Object o) : m_value(std::move(o)) {}

    // ─── Type queries ────────────────────────────────────────────────────
    bool IsNull()   const { return std::holds_alternative<std::nullptr_t>(m_value); }
    bool IsBool()   const { return std::holds_alternative<bool>(m_value); }
    bool IsNumber() const { return std::holds_alternative<double>(m_value); }
    bool IsString() const { return std::holds_alternative<std::string>(m_value); }
    bool IsArray()  const { return std::holds_alternative<Array>(m_value); }
    bool IsObject() const { return std::holds_alternative<Object>(m_value); }

    // ─── Accessors ───────────────────────────────────────────────────────
    bool AsBool(bool def = false) const {
        if (auto* b = std::get_if<bool>(&m_value)) return *b;
        if (auto* n = std::get_if<double>(&m_value)) return *n != 0.0;
        if (auto* s = std::get_if<std::string>(&m_value)) {
            return *s == "true" || *s == "1";
        }
        return def;
    }

    double AsNumber(double def = 0.0) const {
        if (auto* n = std::get_if<double>(&m_value)) return *n;
        if (auto* b = std::get_if<bool>(&m_value)) return *b ? 1.0 : 0.0;
        if (auto* s = std::get_if<std::string>(&m_value)) return std::atof(s->c_str());
        return def;
    }

    int AsInt(int def = 0) const { return static_cast<int>(AsNumber(static_cast<double>(def))); }
    float AsFloat(float def = 0.0f) const { return static_cast<float>(AsNumber(static_cast<double>(def))); }

    const std::string& AsString() const {
        static const std::string kEmpty;
        if (auto* s = std::get_if<std::string>(&m_value)) return *s;
        return kEmpty;
    }

    std::string AsString(const std::string& def) const {
        if (auto* s = std::get_if<std::string>(&m_value)) return *s;
        return def;
    }

    const Array& AsArray() const {
        static const Array kEmpty;
        if (auto* a = std::get_if<Array>(&m_value)) return *a;
        return kEmpty;
    }

    const Object& AsObject() const {
        static const Object kEmpty;
        if (auto* o = std::get_if<Object>(&m_value)) return *o;
        return kEmpty;
    }

    // ─── Object access ───────────────────────────────────────────────────
    bool Has(std::string_view key) const {
        const auto* o = std::get_if<Object>(&m_value);
        return o && o->find(key) != o->end();
    }

    const Json& operator[](std::string_view key) const {
        static const Json kNull;
        const auto* o = std::get_if<Object>(&m_value);
        if (!o) return kNull;
        auto it = o->find(key);
        return it == o->end() ? kNull : it->second;
    }

    Json& operator[](std::string key) {
        auto& o = std::get<Object>(m_value);
        return o[key];
    }

    // ─── Serialization ───────────────────────────────────────────────────
    std::string Dump() const {
        std::string out;
        out.reserve(256);
        DumpInto(out);
        return out;
    }

    // ─── Parsing ─────────────────────────────────────────────────────────
    static std::optional<Json> Parse(std::string_view text) {
        Parser parser{text};
        auto value = parser.ParseValue();
        if (!value) return std::nullopt;
        parser.SkipWs(); // allow EOF (returns false at end — that's fine)
        if (parser.pos != text.size()) return std::nullopt;
        return value;
    }

private:
    static void AppendUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    static void WriteString(std::string& out, std::string_view s) {
        out.push_back('"');
        for (char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b";  break;
                case '\f': out += "\\f";  break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    } else {
                        out.push_back(c);
                    }
            }
        }
        out.push_back('"');
    }

    void DumpInto(std::string& out) const {
        if (auto* n = std::get_if<std::nullptr_t>(&m_value)) {
            (void)n;
            out += "null";
        } else if (auto* b = std::get_if<bool>(&m_value)) {
            out += *b ? "true" : "false";
        } else if (auto* d = std::get_if<double>(&m_value)) {
            if (std::isfinite(*d)) {
                if (*d == std::floor(*d) && std::fabs(*d) < 1e15) {
                    out += std::to_string(static_cast<long long>(*d));
                } else {
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "%.10g", *d);
                    out += buf;
                }
            } else {
                out += "null";
            }
        } else if (auto* s = std::get_if<std::string>(&m_value)) {
            WriteString(out, *s);
        } else if (auto* a = std::get_if<Array>(&m_value)) {
            out.push_back('[');
            for (size_t i = 0; i < a->size(); ++i) {
                if (i) out.push_back(',');
                (*a)[i].DumpInto(out);
            }
            out.push_back(']');
        } else if (auto* o = std::get_if<Object>(&m_value)) {
            out.push_back('{');
            bool first = true;
            for (const auto& [k, v] : *o) {
                if (!first) out.push_back(',');
                first = false;
                WriteString(out, k);
                out.push_back(':');
                v.DumpInto(out);
            }
            out.push_back('}');
        }
    }

    struct Parser {
        std::string_view text;
        size_t pos = 0;
        int depth = 0;
        static constexpr int kMaxDepth = 64;

        bool SkipWs() {
            while (pos < text.size() &&
                   (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' || text[pos] == '\r')) {
                ++pos;
            }
            return pos < text.size();
        }

        bool Consume(char c) {
            if (pos < text.size() && text[pos] == c) {
                ++pos;
                return true;
            }
            return false;
        }

        std::optional<Json> ParseValue() {
            if (!SkipWs()) return std::nullopt;
            switch (text[pos]) {
                case '{': return ParseObject();
                case '[': return ParseArray();
                case '"': return ParseString();
                case 't': case 'f': return ParseBool();
                case 'n': return ParseNull();
                default:  return ParseNumber();
            }
        }

        std::optional<Json> ParseObject() {
            if (++depth > kMaxDepth) return std::nullopt;
            if (!Consume('{')) return std::nullopt;
            Object obj;
            if (Consume('}')) {
                --depth;
                return Json(std::move(obj));
            }
            for (;;) {
                if (!SkipWs() || text[pos] != '"') return std::nullopt;
                auto key = ParseStringRaw();
                if (!key) return std::nullopt;
                if (!SkipWs() || !Consume(':')) return std::nullopt;
                auto value = ParseValue();
                if (!value) return std::nullopt;
                obj.emplace(*key, std::move(*value));
                if (!SkipWs()) return std::nullopt;
                if (Consume(',')) continue;
                if (Consume('}')) break;
                return std::nullopt;
            }
            --depth;
            return Json(std::move(obj));
        }

        std::optional<Json> ParseArray() {
            if (++depth > kMaxDepth) return std::nullopt;
            if (!Consume('[')) return std::nullopt;
            Array arr;
            if (Consume(']')) {
                --depth;
                return Json(std::move(arr));
            }
            for (;;) {
                auto value = ParseValue();
                if (!value) return std::nullopt;
                arr.emplace_back(std::move(*value));
                if (!SkipWs()) return std::nullopt;
                if (Consume(',')) continue;
                if (Consume(']')) break;
                return std::nullopt;
            }
            --depth;
            return Json(std::move(arr));
        }

        std::optional<Json> ParseString() {
            auto raw = ParseStringRaw();
            if (!raw) return std::nullopt;
            return Json(std::move(*raw));
        }

        std::optional<std::string> ParseStringRaw() {
            if (!Consume('"')) return std::nullopt;
            std::string out;
            while (pos < text.size()) {
                char c = text[pos++];
                if (c == '"') return out;
                if (c != '\\') {
                    out.push_back(c);
                    continue;
                }
                if (pos >= text.size()) return std::nullopt;
                char e = text[pos++];
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        uint32_t cp = 0;
                        if (!ParseHex4(cp)) return std::nullopt;
                        // Surrogate pair
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            if (pos + 1 < text.size() && text[pos] == '\\' && text[pos + 1] == 'u') {
                                pos += 2;
                                uint32_t low = 0;
                                if (!ParseHex4(low)) return std::nullopt;
                                if (low >= 0xDC00 && low <= 0xDFFF) {
                                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                                }
                            }
                        }
                        AppendUtf8(out, cp);
                        break;
                    }
                    default:
                        return std::nullopt;
                }
            }
            return std::nullopt;
        }

        bool ParseHex4(uint32_t& out) {
            if (pos + 4 > text.size()) return false;
            uint32_t v = 0;
            for (int i = 0; i < 4; ++i) {
                char c = text[pos++];
                v <<= 4;
                if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
                else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
                else return false;
            }
            out = v;
            return true;
        }

        std::optional<Json> ParseBool() {
            if (text.substr(pos, 4) == "true") { pos += 4; return Json(true); }
            if (text.substr(pos, 5) == "false") { pos += 5; return Json(false); }
            return std::nullopt;
        }

        std::optional<Json> ParseNull() {
            if (text.substr(pos, 4) == "null") { pos += 4; return Json(nullptr); }
            return std::nullopt;
        }

        std::optional<Json> ParseNumber() {
            size_t start = pos;
            if (pos < text.size() && text[pos] == '-') ++pos;
            size_t digits = pos;
            while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
            if (pos == digits) return std::nullopt;
            if (pos < text.size() && text[pos] == '.') {
                ++pos;
                size_t frac = pos;
                while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
                if (pos == frac) return std::nullopt;
            }
            if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
                ++pos;
                if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) ++pos;
                size_t exp = pos;
                while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
                if (pos == exp) return std::nullopt;
            }
            std::string number(text.substr(start, pos - start));
            return Json(std::strtod(number.c_str(), nullptr));
        }
    };

    Value m_value;
};

} // namespace rex::runtime

``

## src/platform/windows/WorkerWManager.cpp

``cpp
#include "WorkerWManager.hpp"
#include <shared/Logger.hpp>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <cstdio>

namespace rex::platform::windows {

namespace {
    std::wstring GetWindowClassName(HWND hwnd) {
        wchar_t buffer[256];
        if (GetClassNameW(hwnd, buffer, 256)) {
            return std::wstring(buffer);
        }
        return L"";
    }

    std::string WidenToUtf8(const std::wstring& wide) {
        if (wide.empty()) return {};
        int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1,
                                       nullptr, 0, nullptr, nullptr);
        if (size <= 1) return {};
        std::string utf8(size - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1,
                            utf8.data(), size, nullptr, nullptr);
        return utf8;
    }

    // Deterministic desktop-shell layout we enforce:
    //
    //   Progman
    //   ├── WorkerW (wallpaper layer)     ← REX render window goes here
    //   │    └── [no SHELLDLL_DefView]
    //   └── WorkerW (icon layer)
    //        └── SHELLDLL_DefView
    //             └── Desktop icons
    //
    // Windows 11 (22H2+) may attach SHELLDLL_DefView directly to Progman:
    //
    //   Progman
    //   ├── SHELLDLL_DefView
    //   └── WorkerW (wallpaper layer)
    //
    // Either way the rule is identical: the wallpaper render window must
    // be parented into a WorkerW that does NOT contain SHELLDLL_DefView,
    // otherwise it renders ABOVE the desktop icons.
    //
    // Some shells (Explorer Patcher, Windhawk, old wallpaper engines) keep
    // an opaque "paint layer" WorkerW as a Progman child that redraws the
    // OS wallpaper bitmap. It is invisible to EnumWindows (child window),
    // so discovery must look at Progman children directly, and any layer
    // we create has to be inserted ABOVE it or it gets covered.

    constexpr UINT WM_SPAWNWORKERW = 0x052C;

    // Returns the topmost Progman-child WorkerW that paints the OS
    // wallpaper: a WorkerW that contains no SHELLDLL_DefView and is not
    // the window passed as `exclude`. nullptr when none exists.
    HWND FindPaintLayerWorkerW(HWND progman, HWND exclude = nullptr) {
        if (!progman) return nullptr;
        HWND child = FindWindowExW(progman, nullptr, L"WorkerW", nullptr);
        while (child) {
            if (child != exclude) {
                HWND defView = FindWindowExW(child, nullptr,
                                             L"SHELLDLL_DefView", nullptr);
                if (!defView) {
                    return child;  // topmost non-icon WorkerW child
                }
            }
            child = FindWindowExW(progman, child, L"WorkerW", nullptr);
        }
        return nullptr;
    }
}

WorkerWManager::WorkerWManager() = default;
WorkerWManager::~WorkerWManager() {
    StopMonitor();
    Release();
}

shared::Result<void> WorkerWManager::Acquire() {
    std::lock_guard<std::mutex> opLock(m_opMutex);
    if (m_acquired) {
        return shared::Result<void>(shared::ErrorCode::AlreadyInitialized,
                                    "WorkerW already acquired");
    }

    REX_LOG_INFO("WorkerW", "Acquiring WorkerW...");

    m_syntheticWorkerW = false;
    m_workerW = FindBlankWorkerW();
    if (!m_workerW) {
        return shared::Result<void>(shared::ErrorCode::NotFound,
                                    "Failed to find wallpaper WorkerW");
    }

    REX_LOG_INFO("WorkerW", "Found wallpaper WorkerW: {}", fmt::ptr(m_workerW));

    m_width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    m_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    m_virtualLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
    m_virtualTop  = GetSystemMetrics(SM_YVIRTUALSCREEN);
    REX_LOG_INFO("WorkerW", "Virtual desktop: {}x{} offset ({}, {})",
                 m_width, m_height, m_virtualLeft, m_virtualTop);

    m_renderWindow = CreateRenderWindow(m_workerW, m_width, m_height);
    if (!m_renderWindow) {
        return shared::Result<void>(shared::ErrorCode::UnknownError,
                                    "Failed to create render window");
    }

    ReparentToWorkerW(m_renderWindow, m_workerW);

    if (!VerifyWallpaperLayer()) {
        m_state.store(DesktopHostState::Suspect, std::memory_order_release);
        return shared::Result<void>(shared::ErrorCode::UnknownError,
                                    "Wallpaper layer verification failed");
    }

    m_acquired = true;
    m_state.store(DesktopHostState::Normal, std::memory_order_release);
    REX_LOG_INFO("WorkerW", "WorkerW acquired successfully");
    return shared::Result<void>();
}

void WorkerWManager::Release() {
    std::lock_guard<std::mutex> opLock(m_opMutex);
    if (!m_acquired) return;

    REX_LOG_INFO("WorkerW", "Releasing WorkerW...");

    if (m_renderWindow) {
        DestroyWindow(m_renderWindow);
        m_renderWindow = nullptr;
    }

    if (m_syntheticWorkerW && m_workerW) {
        REX_LOG_INFO("WorkerW", "Destroying synthetic WorkerW: {}",
                     fmt::ptr(m_workerW));
        DestroyWindow(m_workerW);
        m_syntheticWorkerW = false;
    }

    m_workerW = nullptr;
    m_acquired = false;
    REX_LOG_INFO("WorkerW", "WorkerW released");
}

void WorkerWManager::StartMonitor(WorkerWRecoveryCallback onRecovery) {
    if (m_monitoring) return;

    m_recoveryCallback = std::move(onRecovery);
    m_monitoring = true;
    m_monitorThread = std::thread(&WorkerWManager::MonitorExplorer, this);

    REX_LOG_INFO("WorkerW", "Explorer monitor started");
}

void WorkerWManager::StopMonitor() {
    if (!m_monitoring) return;
    m_monitoring = false;
    if (m_monitorThread.joinable()) {
        m_monitorThread.join();
    }
    REX_LOG_INFO("WorkerW", "Explorer monitor stopped");
}

// ─── Layer verification ────────────────────────────────────────────────────
//
// Enforces the invariants the user-visible behaviour depends on:
//   1. the render window exists and is WS_CHILD (never top-level popup)
//   2. its parent is a WorkerW
//   3. that WorkerW does NOT contain SHELLDLL_DefView (icon layer)
//   4. neither the render window nor the WorkerW is WS_EX_TOPMOST
//   5. the render window covers the WorkerW client area (virtual desktop)
//
// Returns true when every invariant holds.

bool WorkerWManager::VerifyWallpaperLayer() {
    if (!m_renderWindow) {
        REX_LOG_ERROR("WorkerW", "Verify failed: no render window");
        return false;
    }
    if (!m_workerW) {
        REX_LOG_ERROR("WorkerW", "Verify failed: no WorkerW");
        return false;
    }

    LONG_PTR style = GetWindowLongPtrW(m_renderWindow, GWL_STYLE);
    LONG_PTR exStyle = GetWindowLongPtrW(m_renderWindow, GWL_EXSTYLE);

    if (!(style & WS_CHILD)) {
        REX_LOG_ERROR("WorkerW",
            "Verify failed: render window is not WS_CHILD (style=0x{:X})",
            static_cast<unsigned long long>(style));
        return false;
    }

    if (exStyle & WS_EX_TOPMOST) {
        REX_LOG_ERROR("WorkerW",
            "Verify failed: render window is WS_EX_TOPMOST");
        return false;
    }

    HWND actualParent = GetParent(m_renderWindow);
    if (actualParent != m_workerW) {
        REX_LOG_ERROR("WorkerW",
            "Verify failed: render window parent {:p} != wallpaper WorkerW {:p}",
            fmt::ptr(actualParent), fmt::ptr(m_workerW));
        return false;
    }

    HWND defView = FindWindowExW(m_workerW, nullptr, L"SHELLDLL_DefView", nullptr);
    if (defView) {
        REX_LOG_ERROR("WorkerW",
            "Verify failed: selected WorkerW {:p} contains SHELLDLL_DefView "
            "(icon layer) — wallpaper would render above desktop icons",
            fmt::ptr(m_workerW));
        return false;
    }

    // Z-order invariant: the wallpaper paint layer (a Progman-child WorkerW
    // that redraws the OS wallpaper bitmap) must never sit above our layer,
    // or the wallpaper is covered. Walk siblings upward from our WorkerW;
    // reaching the paint layer before the top of the list means it covers us.
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (progman) {
        HWND paintLayer = FindPaintLayerWorkerW(progman, m_workerW);
        if (paintLayer) {
            HWND above = GetWindow(m_workerW, GW_HWNDPREV);
            while (above) {
                if (above == paintLayer) {
                    REX_LOG_ERROR("WorkerW",
                        "Verify failed: wallpaper paint layer {:p} is above "
                        "our WorkerW {:p} — wallpaper would be covered",
                        fmt::ptr(paintLayer), fmt::ptr(m_workerW));
                    return false;
                }
                above = GetWindow(above, GW_HWNDPREV);
            }
        }
    }

    LONG_PTR wwExStyle = GetWindowLongPtrW(m_workerW, GWL_EXSTYLE);
    if (wwExStyle & WS_EX_TOPMOST) {
        REX_LOG_WARN("WorkerW",
            "WorkerW {:p} is WS_EX_TOPMOST — desktop icons may be obscured",
            fmt::ptr(m_workerW));
    }

    REX_LOG_DEBUG("WorkerW", "Verify OK: render window parented to wallpaper "
                             "WorkerW {:p}, WS_CHILD, not topmost",
                  fmt::ptr(m_workerW));
    return true;
}

// ─── Reacquire (shared by Explorer-restart and layer-invalid paths) ────────

bool WorkerWManager::ReacquireWorkerW() {
    std::lock_guard<std::mutex> opLock(m_opMutex);
    m_state.store(DesktopHostState::Repairing, std::memory_order_release);
    REX_LOG_INFO("WorkerW", "Repairing wallpaper layer (reacquire)...");
    if (m_renderWindow) {
        DestroyWindow(m_renderWindow);
        m_renderWindow = nullptr;
    }
    m_workerW = nullptr;
    m_acquired = false;

    HWND workerW = FindBlankWorkerW();
    if (!workerW) {
        m_state.store(DesktopHostState::Suspect, std::memory_order_release);
        REX_LOG_ERROR("WorkerW", "Reacquire failed: no wallpaper WorkerW");
        return false;
    }

    m_workerW = workerW;
    m_width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    m_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    m_virtualLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
    m_virtualTop  = GetSystemMetrics(SM_YVIRTUALSCREEN);

    m_renderWindow = CreateRenderWindow(m_workerW, m_width, m_height);
    if (!m_renderWindow) {
        m_state.store(DesktopHostState::Suspect, std::memory_order_release);
        REX_LOG_ERROR("WorkerW", "Reacquire failed: render window creation");
        return false;
    }

    ReparentToWorkerW(m_renderWindow, m_workerW);

    // P10: verify BEFORE declaring success — the monitor loop treats a
    // true return as healthy, so an unverified repair could leave the
    // render window above the icons for a full check interval.
    if (!VerifyWallpaperLayer()) {
        m_acquired = false;
        m_state.store(DesktopHostState::Suspect, std::memory_order_release);
        REX_LOG_ERROR("WorkerW", "Reacquire failed: post-repair verification");
        return false;
    }

    m_acquired = true;
    m_state.store(DesktopHostState::Verified, std::memory_order_release);
    REX_LOG_INFO("WorkerW", "Wallpaper layer reacquired and verified");

    if (m_recoveryCallback) {
        m_recoveryCallback();
    }
    return true;
}

void WorkerWManager::MonitorExplorer() {
    using namespace std::chrono_literals;

    REX_LOG_INFO("WorkerW", "Monitor thread started");

    while (m_monitoring) {
        HWND progman = FindWindowW(L"Progman", nullptr);
        DWORD pid = 0;
        if (progman) {
            GetWindowThreadProcessId(progman, &pid);
        }

        if (pid == 0) {
            // No Explorer yet — wait and retry.
            std::this_thread::sleep_for(2s);
            continue;
        }

        HANDLE hProcess = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (!hProcess) {
            std::this_thread::sleep_for(2s);
            continue;
        }

        REX_LOG_DEBUG("WorkerW", "Monitoring Explorer.exe (PID: {})", pid);

        // Wait for Explorer exit OR the periodic invariant check interval,
        // whichever comes first. This lets us detect Explorer recreating its
        // desktop windows without us having to restart the wait every time.
        DWORD waitResult = WaitForSingleObject(hProcess, 3000);
        CloseHandle(hProcess);

        if (waitResult == WAIT_OBJECT_0) {
            REX_LOG_WARN("WorkerW", "Explorer.exe terminated. "
                         "Waiting for restart...");

            while (m_monitoring) {
                HWND newProgman = FindWindowW(L"Progman", nullptr);
                if (newProgman && newProgman != progman) {
                    REX_LOG_INFO("WorkerW", "Explorer restarted. "
                                 "Reacquiring WorkerW...");
                    if (!ReacquireWorkerW()) {
                        REX_LOG_ERROR("WorkerW",
                            "Reacquire after Explorer restart failed");
                    }
                    break;
                }
                std::this_thread::sleep_for(500ms);
            }
            continue;
        }

        // ── Periodic invariant check (Explorer alive) ────────────────────
        // Explorer can recreate the desktop window structure while the
        // process stays alive (display mode change, shell refresh, other
        // wallpaper apps, WE applying its own wallpaper, ...). If the
        // render window is no longer parented into the wallpaper WorkerW,
        // it can end up above the icons — reacquire before that shows.
        if (m_acquired) {
            bool ok = m_renderWindow && IsWindow(m_renderWindow) &&
                      m_workerW && IsWindow(m_workerW) &&
                      VerifyWallpaperLayer();

            if (!ok) {
                m_state.store(DesktopHostState::Suspect, std::memory_order_release);
                REX_LOG_WARN("WorkerW",
                    "Wallpaper layer invalid (Explorer refreshed windows); "
                    "reacquiring...");
                if (!ReacquireWorkerW()) {
                    REX_LOG_ERROR("WorkerW",
                        "Reacquire after layer invalidation failed");
                }
            }
        }
    }

    REX_LOG_INFO("WorkerW", "Monitor thread ended");
}

// ─── Wallpaper WorkerW discovery ────────────────────────────────────────────
//
// Steps (see header comment for the target hierarchy):
//   1. locate Progman
//   2. force WorkerW creation (WM_SPAWNWORKERW)
//   3. enumerate every top-level WorkerW
//   4. find the icon-layer WorkerW (contains SHELLDLL_DefView)
//   5. select a different WorkerW — the wallpaper layer
//   6. verify selection properties (visible, parent, size)
//   7. resize it to the virtual desktop bounds

HWND WorkerWManager::FindBlankWorkerW() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman) {
        REX_LOG_ERROR("WorkerW", "Progman not found — desktop layer unavailable");
        return nullptr;
    }

    // Force Explorer to spawn the wallpaper WorkerW.
    for (int i = 0; i < 4; i++) {
        SendMessageTimeoutW(progman, WM_SPAWNWORKERW, 0, 0,
                            SMTO_NORMAL, 1000, nullptr);
        Sleep(100);
    }

    int screenW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int screenH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    int virtualLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int virtualTop  = GetSystemMetrics(SM_YVIRTUALSCREEN);

    wchar_t tempDir[MAX_PATH] = {};
    std::string diagPath;
    if (GetTempPathW(MAX_PATH, tempDir)) {
        diagPath = WidenToUtf8(std::wstring(tempDir) + L"REX\\workerw_diag.log");
        CreateDirectoryW((std::wstring(tempDir) + L"REX").c_str(), nullptr);
    }
    FILE* diag = nullptr;
    fopen_s(&diag, diagPath.c_str(), "w");

    #define WDLOG(fmt, ...) do { \
        if (diag) { fprintf(diag, fmt "\n", ##__VA_ARGS__); fflush(diag); } \
    } while(0)

    WDLOG("=== WorkerW Discovery ===");
    WDLOG("Desktop: %dx%d at (%d,%d)", screenW, screenH, virtualLeft, virtualTop);
    WDLOG("Progman: %p", progman);

    struct Candidate {
        HWND   hwnd;
        RECT   rect;
        bool   visible;
        bool   hasDefView;
        HWND   parent;
        bool   parentIsProgman;
        int    area;
        bool   fullScreen;  // covers >= 90% of the virtual desktop
    };

    std::vector<Candidate> workers;

    EnumWindows([](HWND hwnd, LPARAM lparam) -> BOOL {
        if (GetWindowClassName(hwnd) != L"WorkerW") return TRUE;

        auto* vec = reinterpret_cast<std::vector<Candidate>*>(lparam);

        Candidate c;
        c.hwnd = hwnd;
        GetWindowRect(hwnd, &c.rect);
        c.visible = IsWindowVisible(hwnd);
        c.hasDefView = FindWindowExW(hwnd, nullptr,
                                     L"SHELLDLL_DefView", nullptr) != nullptr;
        c.parent = GetParent(hwnd);
        c.parentIsProgman = (GetClassNameW(c.parent, nullptr, 0) > 0) &&
            (GetWindowClassName(c.parent) == L"Progman");
        c.area = (c.rect.right - c.rect.left) * (c.rect.bottom - c.rect.top);
        c.fullScreen = false;
        vec->push_back(c);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&workers));

    // ── Classify ─────────────────────────────────────────────────────────
    HWND iconWorkerW = nullptr;   // the one containing SHELLDLL_DefView
    for (const auto& w : workers) {
        WDLOG("  WorkerW %p: %dx%d visible=%d defView=%d parent=%p (%s)",
              w.hwnd,
              w.rect.right - w.rect.left, w.rect.bottom - w.rect.top,
              w.visible ? 1 : 0, w.hasDefView ? 1 : 0, w.parent,
              w.parentIsProgman ? "progman" : "other");
        if (w.hasDefView) iconWorkerW = w.hwnd;
    }

    // ── Select the wallpaper layer ───────────────────────────────────────
    // Rule: never select the icon-layer WorkerW. Prefer a visible WorkerW
    // parented under Progman that covers the virtual desktop; otherwise the
    // largest visible one. (Win11 may expose only the wallpaper WorkerW.)
    HWND target = nullptr;
    int bestScore = -1;
    int bestArea = 0;

    for (const auto& w : workers) {
        if (w.hwnd == iconWorkerW) continue;   // invariant: never the icons
        if (!w.visible) continue;

        bool fullScreen = (w.rect.right - w.rect.left >= screenW * 90 / 100 &&
                           w.rect.bottom - w.rect.top >= screenH * 90 / 100);

        int score = 0;
        if (w.parentIsProgman && fullScreen) score = 100;
        else if (w.parentIsProgman)          score = 80;
        else if (fullScreen)                 score = 70;
        else                                 score = 50;

        // Tie-break by size so multi-monitor setups pick the window that
        // spans the virtual desktop.
        if (score > bestScore) {
            bestScore = score;
            target = w.hwnd;
            bestArea = w.area;
        } else if (score == bestScore && w.area > bestArea) {
            target = w.hwnd;
            bestArea = w.area;
        }
    }

    if (target) {
        WDLOG("Selected wallpaper WorkerW: %p (score=%d)", target, bestScore);
    } else {
        WDLOG("No suitable WorkerW found — creating synthetic WorkerW");
    }

    // ── Resize the selected WorkerW to the virtual desktop ───────────────
    // A worker-window sized to the work area clips the wallpaper to part of
    // the screen; DPI mismatches (virtualized vs physical) also leave stale
    // sizes. Always assert the full virtual desktop bounds here.
    if (target) {
        RECT r;
        GetWindowRect(target, &r);
        int curW = r.right - r.left;
        int curH = r.bottom - r.top;
        bool coversDesktop = (curW >= screenW && curH >= screenH);

        // If the icon WorkerW is our sibling, insert directly below it so
        // the wallpaper stays behind the icons even if Explorer reorders
        // windows later. The anchor is either the DefView (Win11: child of
        // Progman) or the icon WorkerW (Win10).
        HWND siblingAnchor = nullptr;
        HWND defView = FindWindowExW(progman, nullptr,
                                     L"SHELLDLL_DefView", nullptr);
        if (!defView && iconWorkerW &&
            GetParent(iconWorkerW) == GetParent(target)) {
            siblingAnchor = iconWorkerW;
        } else if (defView) {
            siblingAnchor = defView;
        }

        if (!coversDesktop || siblingAnchor) {
            WDLOG("Resizing WorkerW %p: %dx%d -> %dx%d anchor=%p",
                  target, curW, curH, screenW, screenH, siblingAnchor);
            SetWindowPos(target,
                         siblingAnchor ? siblingAnchor : HWND_BOTTOM,
                         virtualLeft, virtualTop, screenW, screenH,
                         SWP_NOACTIVATE | SWP_FRAMECHANGED);
            Sleep(100);
        }
    }

    if (!target) {
        // ── Synthetic WorkerW under Progman (last resort) ───────────────
        REX_LOG_WARN("WorkerW", "Creating synthetic WorkerW under Progman");
        WNDCLASSW wc = {};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"WorkerW";
        RegisterClassW(&wc);

        target = CreateWindowExW(
            0, L"WorkerW", L"",
            WS_CHILD | WS_VISIBLE,
            0, 0, screenW, screenH,
            progman, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        if (target) {
            m_syntheticWorkerW = true;
            REX_LOG_INFO("WorkerW", "Created synthetic WorkerW: {}",
                         fmt::ptr(target));

            // Insert above the explorer wallpaper-paint layer (if any) so
            // the OS wallpaper bitmap cannot cover us, but still below
            // SHELLDLL_DefView so desktop icons stay on top. hWndInsertAfter
            // is the window that PRECEDES (sits above) ours, so when a
            // paint layer exists we anchor to the sibling above it. When
            // no paint layer exists, HWND_BOTTOM is the correct desktop
            // position.
            HWND insertAfter = HWND_BOTTOM;
            HWND paintLayer = FindPaintLayerWorkerW(progman, target);
            if (paintLayer) {
                HWND abovePaint = GetWindow(paintLayer, GW_HWNDPREV);
                insertAfter = abovePaint ? abovePaint : HWND_TOP;
                WDLOG("Synthetic WorkerW: paint layer %p, insertAfter=%p%s",
                      paintLayer, insertAfter,
                      abovePaint ? " (above paint layer)" : " (HWND_TOP)");
            } else {
                WDLOG("Synthetic WorkerW: no paint layer, insertAfter=HWND_BOTTOM");
            }
            SetWindowPos(target, insertAfter,
                         0, 0, screenW, screenH,
                         SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
    }

    WDLOG("Selected WorkerW: %p", target);
    if (diag) fclose(diag);
    #undef WDLOG

    return target;
}

HWND WorkerWManager::CreateRenderWindow(HWND parent, int width, int height) {
    const wchar_t CLASS_NAME[] = L"REXRenderWindow";
    HINSTANCE hInst = GetModuleHandleW(nullptr);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = CreateSolidBrush(RGB(0, 0, 0));

    if (!RegisterClassW(&wc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            REX_LOG_ERROR("WorkerW", "RegisterClassW failed: {}", err);
            return nullptr;
        }
    }

    // Deliberately NOT WS_EX_TOPMOST: the window must stay inside the
    // desktop layer. Created as a top-level popup first (hidden) so it can
    // be parented into the WorkerW in ReparentToWorkerW; never shown
    // between those steps.
    HWND hwnd = CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
        CLASS_NAME,
        L"REX Render",
        WS_POPUP,
        m_virtualLeft, m_virtualTop, width, height,
        nullptr,
        nullptr,
        hInst,
        nullptr);

    if (!hwnd) {
        REX_LOG_ERROR("WorkerW", "CreateWindowExW failed: {}", GetLastError());
        return nullptr;
    }

    return hwnd;
}

void WorkerWManager::ReparentToWorkerW(HWND renderWindow, HWND workerW) {
    if (!renderWindow || !workerW) return;

    SetParent(renderWindow, workerW);

    LONG_PTR style = GetWindowLongPtrW(renderWindow, GWL_STYLE);
    style = (style & ~WS_POPUP) | WS_CHILD | WS_VISIBLE;
    SetWindowLongPtrW(renderWindow, GWL_STYLE, style);

    // Keep the extended style minimal: no WS_EX_TOPMOST ever — the render
    // window belongs inside the desktop layer, not in the top-level Z-order.
    SetWindowLongPtrW(renderWindow, GWL_EXSTYLE,
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT);

    SetWindowPos(renderWindow, HWND_BOTTOM,
                 0, 0,
                 m_width, m_height,
                 SWP_NOACTIVATE | SWP_FRAMECHANGED);

    // Verify final size: if the WorkerW client area is smaller than the
    // virtual desktop (work-area-only WorkerW), shrink the render window
    // to match so nothing is clipped or stretched. Explorer can also
    // resize windows after reparenting, so assert the rect here.
    RECT parentClient;
    if (GetClientRect(workerW, &parentClient)) {
        int cw = parentClient.right - parentClient.left;
        int ch = parentClient.bottom - parentClient.top;
        if (cw > 0 && ch > 0 && (cw != m_width || ch != m_height)) {
            REX_LOG_WARN("WorkerW",
                "Render window {}x{} does not match WorkerW client {}x{}; correcting",
                m_width, m_height, cw, ch);
            SetWindowPos(renderWindow, HWND_BOTTOM, 0, 0, cw, ch,
                SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
    }

    RECT finalRect;
    if (GetWindowRect(renderWindow, &finalRect)) {
        REX_LOG_INFO("WorkerW",
            "Render window final rect: {}x{} at ({}, {})",
            finalRect.right - finalRect.left,
            finalRect.bottom - finalRect.top,
            finalRect.left, finalRect.top);
    }

    // Post-reparent invariant checks (belt and braces — the monitor thread
    // re-verifies periodically too).
    LONG_PTR ex = GetWindowLongPtrW(renderWindow, GWL_EXSTYLE);
    if (ex & WS_EX_TOPMOST) {
        REX_LOG_ERROR("WorkerW",
            "Render window is WS_EX_TOPMOST after reparent — stripping");
        SetWindowLongPtrW(renderWindow, GWL_EXSTYLE,
            ex & ~WS_EX_TOPMOST);
    }

    HWND parent = GetParent(renderWindow);
    if (parent != workerW) {
        REX_LOG_ERROR("WorkerW",
            "Render window parent {:p} != target WorkerW {:p} after reparent",
            fmt::ptr(parent), fmt::ptr(workerW));
    } else {
        REX_LOG_DEBUG("WorkerW",
            "Render window parent verified: wallpaper WorkerW {:p}",
            fmt::ptr(workerW));
    }

    ShowWindow(renderWindow, SW_SHOW);
    UpdateWindow(renderWindow);

    REX_LOG_DEBUG("WorkerW",
        "Window reparented to wallpaper WorkerW and positioned at bottom");
}

LRESULT CALLBACK WorkerWManager::WndProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {

    switch (msg) {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

} // namespace rex::platform::windows

``

## src/platform/windows/WorkerWManager.hpp

``cpp
#pragma once

#include <windows.h>
#include <shared/Result.hpp>
#include <string>
#include <atomic>
#include <mutex>
#include <thread>
#include <functional>

namespace rex::platform::windows {

using WorkerWRecoveryCallback = std::function<void()>;

// ─── Desktop-host lifecycle state (P10 diagnostics) ────────────────────────
// Tracked across Explorer restarts / layer invalidation so crash logs and
// telemetry can answer "was the wallpaper layer healthy?" without guessing.
enum class DesktopHostState {
    Normal,     // acquired, last verification passed
    Suspect,    // an invariant check failed; repair pending or failed
    Repairing,  // ReacquireWorkerW in progress
    Verified,   // repair finished AND VerifyWallpaperLayer passed
};

class WorkerWManager {
public:
    WorkerWManager();
    ~WorkerWManager();

    shared::Result<void> Acquire();
    void Release();

    void StartMonitor(WorkerWRecoveryCallback onRecovery);
    void StopMonitor();

    HWND GetWorkerW()      const { return m_workerW; }
    HWND GetRenderWindow() const { return m_renderWindow; }
    bool IsValid()         const { return m_workerW != nullptr && m_renderWindow != nullptr; }
    bool IsAcquired()      const { return m_acquired; }
    DesktopHostState GetState() const { return m_state.load(std::memory_order_acquire); }

private:
    HWND FindBlankWorkerW();
    HWND CreateRenderWindow(HWND parent, int width, int height);
    void ReparentToWorkerW(HWND renderWindow, HWND workerW);
    void MonitorExplorer();
    bool ReacquireWorkerW();
    bool VerifyWallpaperLayer();

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    HWND m_workerW = nullptr;
    HWND m_renderWindow = nullptr;
    int m_width = 0;
    int m_height = 0;
    int m_virtualLeft = 0;
    int m_virtualTop = 0;
    bool m_acquired = false;
    bool m_syntheticWorkerW = false;

    std::atomic<bool> m_monitoring{false};
    std::thread m_monitorThread;
    WorkerWRecoveryCallback m_recoveryCallback;
    std::atomic<DesktopHostState> m_state{DesktopHostState::Normal};
    // Serializes Acquire/Release/Reacquire. Without it, the monitor
    // thread repairing the layer races the render thread's device-loss
    // reinit (both mutate the same HWNDs) — observed as a permanent
    // mutex-wait hang during Explorer restarts (crash.log 7.6MB of dumps).
    mutable std::mutex m_opMutex;
};

} // namespace rex::platform::windows

``

## src/platform/windows/WorkerWHost.cpp

``cpp
#include "WorkerWHost.hpp"
#include <shared/Logger.hpp>

namespace rex::platform::windows {

WorkerWHost::WorkerWHost()
    : m_manager(std::make_unique<WorkerWManager>()) {
}

WorkerWHost::~WorkerWHost() {
    Release();
}

shared::Result<void> WorkerWHost::Acquire() {
    if (m_manager->IsAcquired()) {
        return shared::Result<void>(shared::ErrorCode::AlreadyInitialized,
            "WorkerWHost already acquired");
    }

    REX_LOG_INFO("WorkerWHost", "Acquiring WorkerW desktop host...");

    auto result = m_manager->Acquire();
    if (result.IsError()) {
        REX_LOG_ERROR("WorkerWHost",
            "WorkerWManager::Acquire failed: {}", result.Error().message);
        return result;
    }

    HWND workerW = m_manager->GetWorkerW();
    if (!workerW) {
        m_manager->Release();
        return shared::Result<void>(shared::ErrorCode::InvalidState,
            "WorkerW handle is null after successful WorkerWManager::Acquire()");
    }

    if (!IsWindow(workerW)) {
        m_manager->Release();
        return shared::Result<void>(shared::ErrorCode::InvalidState,
            "WorkerW handle is not a valid window");
    }

    RECT wwRect;
    if (!GetWindowRect(workerW, &wwRect)) {
        m_manager->Release();
        return shared::Result<void>(shared::ErrorCode::UnknownError,
            "Failed to get WorkerW window dimensions");
    }

    m_width  = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    m_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    int wwW = wwRect.right - wwRect.left;
    int wwH = wwRect.bottom - wwRect.top;

    if (wwW < m_width / 2 || wwH < m_height / 2) {
        REX_LOG_WARN("WorkerWHost",
            "WorkerW too small for wallpaper: {}x{} (desktop {}x{})",
            wwW, wwH, m_width, m_height);
        m_manager->Release();
        return shared::Result<void>(shared::ErrorCode::InvalidState,
            "WorkerW dimensions too small for wallpaper rendering. "
            "The WorkerW found is not the desktop wallpaper layer.");
    }

    HWND renderWindow = m_manager->GetRenderWindow();
    if (!renderWindow || !IsWindow(renderWindow)) {
        m_manager->Release();
        return shared::Result<void>(shared::ErrorCode::InvalidState,
            "Render window is not a valid window after WorkerW acquisition");
    }

    if (!IsWindowVisible(renderWindow)) {
        REX_LOG_WARN("WorkerWHost",
            "Render window exists but is not visible: {}",
            fmt::ptr(renderWindow));
    }

    REX_LOG_INFO("WorkerWHost",
        "WorkerW acquired: parent {} ({}x{}), render window {}",
        fmt::ptr(workerW), wwW, wwH, fmt::ptr(renderWindow));

    return shared::Result<void>();
}

void WorkerWHost::Release() {
    if (!m_manager->IsAcquired()) return;

    REX_LOG_DEBUG("WorkerWHost", "Releasing WorkerW desktop host...");
    m_manager->Release();
    REX_LOG_INFO("WorkerWHost", "WorkerW released");
}

HWND WorkerWHost::GetRenderWindow() const {
    return m_manager->GetRenderWindow();
}

int  WorkerWHost::GetWidth()  const { return m_width; }
int  WorkerWHost::GetHeight() const { return m_height; }

bool WorkerWHost::IsValid()    const { return m_manager->IsValid(); }
bool WorkerWHost::IsAcquired() const { return m_manager->IsAcquired(); }

std::string_view WorkerWHost::GetHostType() const { return "WorkerW"; }

void WorkerWHost::StartMonitor(std::function<void()> onRecovery) {
    m_manager->StartMonitor(std::move(onRecovery));
}

void WorkerWHost::StopMonitor() {
    m_manager->StopMonitor();
}

} // namespace rex::platform::windows

``

## src/platform/windows/WorkerWHost.hpp

``cpp
#pragma once

#include "IDesktopHost.hpp"
#include "WorkerWManager.hpp"
#include <memory>

namespace rex::platform::windows {

class WorkerWHost : public IDesktopHost {
public:
    WorkerWHost();
    ~WorkerWHost() override;

    shared::Result<void> Acquire() override;
    void Release() override;

    HWND    GetRenderWindow() const override;
    int     GetWidth()  const override;
    int     GetHeight() const override;

    bool    IsValid()    const override;
    bool    IsAcquired() const override;

    std::string_view GetHostType() const override;

    void    StartMonitor(std::function<void()> onRecovery) override;
    void    StopMonitor() override;

private:
    std::unique_ptr<WorkerWManager> m_manager;
    int m_width  = 0;
    int m_height = 0;
};

} // namespace rex::platform::windows

``

## src/platform/windows/ProgmanHost.cpp

``cpp
#include "ProgmanHost.hpp"
#include <shared/Logger.hpp>

#include <chrono>
#include <string>
#include <thread>

namespace rex::platform::windows {

namespace {

    std::wstring GetWindowClass(HWND hwnd) {
        wchar_t buffer[256];
        if (GetClassNameW(hwnd, buffer, 256)) {
            return std::wstring(buffer);
        }
        return L"(unknown)";
    }

}

ProgmanHost::ProgmanHost() {
    m_hInstance = GetModuleHandleW(nullptr);
}

ProgmanHost::~ProgmanHost() {
    StopMonitor();
    Release();
}

shared::Result<void> ProgmanHost::Acquire() {
    if (m_acquired) {
        return shared::Result<void>(shared::ErrorCode::AlreadyInitialized,
            "ProgmanHost already acquired");
    }

    REX_LOG_INFO("ProgmanHost", "Acquiring Progman desktop host...");

    m_progman = FindWindowW(L"Progman", nullptr);
    if (!m_progman) {
        return shared::Result<void>(shared::ErrorCode::NotFound,
            "Progman window not found. Is explorer.exe running?");
    }

    REX_LOG_DEBUG("ProgmanHost", "Found Progman: {}", fmt::ptr(m_progman));

    if (!IsWindow(m_progman)) {
        return shared::Result<void>(shared::ErrorCode::InvalidState,
            "Progman handle is not a valid window");
    }

    if (!IsWindowVisible(m_progman)) {
        return shared::Result<void>(shared::ErrorCode::InvalidState,
            "Progman exists but is not visible - shell may be in an "
            "unusual state");
    }

    RECT progmanRect;
    if (!GetWindowRect(m_progman, &progmanRect)) {
        return shared::Result<void>(shared::ErrorCode::UnknownError,
            "Failed to get Progman window dimensions");
    }

    m_width  = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    m_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    int pW = progmanRect.right - progmanRect.left;
    int pH = progmanRect.bottom - progmanRect.top;

    REX_LOG_DEBUG("ProgmanHost", "Progman rect: {}x{}, desktop: {}x{}",
                  pW, pH, m_width, m_height);

    if (pW < m_width / 2 || pH < m_height / 2) {
        return shared::Result<void>(shared::ErrorCode::InvalidState,
            "Progman window is too small to host wallpaper content. "
            "Expected at least half the desktop dimensions.");
    }

    REX_LOG_INFO("ProgmanHost", "Progman validated: {} ({}x{})",
                 fmt::ptr(m_progman), pW, pH);

    constexpr wchar_t CLASS_NAME[] = L"REXProgmanRender";

    WNDCLASSW wc = {};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = m_hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = CreateSolidBrush(RGB(0, 0, 0));

    if (!RegisterClassW(&wc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            return shared::Result<void>(shared::ErrorCode::UnknownError,
                "RegisterClassW failed for Progman render window (error: " +
                std::to_string(err) + ")");
        }
    }

    int virtualLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int virtualTop  = GetSystemMetrics(SM_YVIRTUALSCREEN);

    m_renderWindow = CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
        CLASS_NAME,
        L"REX Wallpaper",
        WS_POPUP | WS_VISIBLE,
        virtualLeft, virtualTop,
        m_width, m_height,
        nullptr,
        nullptr,
        m_hInstance,
        nullptr);

    if (!m_renderWindow) {
        DWORD err = GetLastError();
        return shared::Result<void>(shared::ErrorCode::UnknownError,
            "CreateWindowExW failed (error: " +
            std::to_string(err) + ")");
    }

    REX_LOG_DEBUG("ProgmanHost", "Render window created: {}",
                  fmt::ptr(m_renderWindow));

    int vl = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vt = GetSystemMetrics(SM_YVIRTUALSCREEN);

    SetWindowPos(m_renderWindow, m_progman,
                 vl, vt, m_width, m_height,
                 SWP_NOACTIVATE | SWP_FRAMECHANGED);

    ShowWindow(m_renderWindow, SW_SHOW);
    UpdateWindow(m_renderWindow);

    REX_LOG_DEBUG("ProgmanHost",
        "Render window positioned below Progman");

    {
        auto wcs2str = [](const wchar_t* wcs) -> std::string {
            if (!wcs) return "(null)";
            int len = WideCharToMultiByte(CP_UTF8, 0, wcs, -1,
                                          nullptr, 0, nullptr, nullptr);
            if (len <= 0) return "(empty)";
            std::string result(static_cast<size_t>(len - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wcs, -1,
                                &result[0], len, nullptr, nullptr);
            return result;
        };

        RECT clientRect;
        GetClientRect(m_renderWindow, &clientRect);
        RECT windowRect;
        GetWindowRect(m_renderWindow, &windowRect);

        HWND parent = GetParent(m_renderWindow);
        wchar_t parentClass[256] = {0};
        wchar_t renderClass[256] = {0};
        if (parent) GetClassNameW(parent, parentClass, 256);
        GetClassNameW(m_renderWindow, renderClass, 256);
        std::string parentClassStr = wcs2str(parentClass);
        std::string renderClassStr = wcs2str(renderClass);

        LONG style   = GetWindowLongW(m_renderWindow, GWL_STYLE);
        LONG exstyle = GetWindowLongW(m_renderWindow, GWL_EXSTYLE);
        BOOL visible = IsWindowVisible(m_renderWindow);

        REX_LOG_INFO("ProgmanHost",
            "|-- Render Window State -----------------------------");
        REX_LOG_INFO("ProgmanHost",
            "| HWND:          {}", fmt::ptr(m_renderWindow));
        REX_LOG_INFO("ProgmanHost",
            "| Class:         {}", renderClassStr);
        REX_LOG_INFO("ProgmanHost",
            "| Parent HWND:   {} (class: {})",
            fmt::ptr(parent), parentClassStr);
        REX_LOG_INFO("ProgmanHost",
            "| Client rect:   {}x{}", clientRect.right, clientRect.bottom);
        REX_LOG_INFO("ProgmanHost",
            "| Window rect:   {}x{} (left={} top={})",
            windowRect.right - windowRect.left,
            windowRect.bottom - windowRect.top,
            windowRect.left, windowRect.top);
        REX_LOG_INFO("ProgmanHost",
            "| Visible:       {}", visible ? "YES" : "NO");
        REX_LOG_INFO("ProgmanHost",
            "| Style:         0x{:08X} (WS_CHILD={})",
            static_cast<uint32_t>(style),
            (style & WS_CHILD) ? "YES" : "NO");
        REX_LOG_INFO("ProgmanHost",
            "| ExStyle:       0x{:08X}", static_cast<uint32_t>(exstyle));
        REX_LOG_INFO("ProgmanHost",
            "| Desktop:       {}x{}", m_width, m_height);

        HWND sibling = GetWindow(m_progman, GW_CHILD);
        int siblingCount = 0;
        REX_LOG_INFO("ProgmanHost",
            "| Progman children:");
        while (sibling && siblingCount < 10) {
            wchar_t sibClass[256] = {0};
            GetClassNameW(sibling, sibClass, 256);
            std::string sibClassStr = wcs2str(sibClass);
            RECT sibRect;
            GetWindowRect(sibling, &sibRect);
            REX_LOG_INFO("ProgmanHost",
                "|   [{:d}] HWND={} class={} rect={}x{} visible={}",
                siblingCount, fmt::ptr(sibling), sibClassStr,
                sibRect.right - sibRect.left,
                sibRect.bottom - sibRect.top,
                IsWindowVisible(sibling) ? "Y" : "N");
            sibling = GetWindow(sibling, GW_HWNDNEXT);
            siblingCount++;
        }
        REX_LOG_INFO("ProgmanHost",
            "|----------------------------------------------");
    }

    if (!IsWindow(m_renderWindow)) {
        Release();
        return shared::Result<void>(shared::ErrorCode::InvalidState,
            "Render window destroyed immediately after creation");
    }

    if (!IsWindowVisible(m_renderWindow)) {
        REX_LOG_WARN("ProgmanHost",
            "Render window is not visible - wallpaper may not appear");
    }

    RECT childRect;
    if (GetWindowRect(m_renderWindow, &childRect)) {
        int cW = childRect.right - childRect.left;
        int cH = childRect.bottom - childRect.top;

        if (cW < m_width / 2 || cH < m_height / 2) {
            Release();
            return shared::Result<void>(shared::ErrorCode::InvalidState,
                "Render window dimensions are unexpectedly small: " +
                std::to_string(cW) + "x" + std::to_string(cH));
        }
    }

    SetWindowPos(m_renderWindow, m_progman,
                 GetSystemMetrics(SM_XVIRTUALSCREEN),
                 GetSystemMetrics(SM_YVIRTUALSCREEN),
                 m_width, m_height,
                 SWP_NOACTIVATE | SWP_FRAMECHANGED);

    m_acquired = true;

    REX_LOG_INFO("ProgmanHost",
        "Progman desktop host acquired: {}x{} window at bottom of "
        "Progman z-order (Progman={}, render={})",
        m_width, m_height,
        fmt::ptr(m_progman), fmt::ptr(m_renderWindow));

    return shared::Result<void>();
}

void ProgmanHost::Release() {
    if (!m_acquired) return;

    REX_LOG_DEBUG("ProgmanHost", "Releasing Progman desktop host...");

    if (m_renderWindow && IsWindow(m_renderWindow)) {
        DestroyWindow(m_renderWindow);
    }
    m_renderWindow = nullptr;
    m_progman      = nullptr;
    m_acquired     = false;

    REX_LOG_INFO("ProgmanHost", "Progman desktop host released");
}

HWND ProgmanHost::GetRenderWindow() const { return m_renderWindow; }
int  ProgmanHost::GetWidth()  const { return m_width; }
int  ProgmanHost::GetHeight() const { return m_height; }

bool ProgmanHost::IsValid() const {
    return m_acquired
        && m_renderWindow != nullptr
        && IsWindow(m_renderWindow);
}

bool ProgmanHost::IsAcquired() const { return m_acquired; }

std::string_view ProgmanHost::GetHostType() const { return "Progman"; }

LRESULT CALLBACK ProgmanHost::WndProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

void ProgmanHost::StartMonitor(std::function<void()> onRecovery) {
    if (m_monitoring) return;

    m_recoveryCallback = std::move(onRecovery);
    m_monitoring.store(true, std::memory_order_release);
    m_monitorThread = std::thread(&ProgmanHost::MonitorShell, this);

    REX_LOG_INFO("ProgmanHost", "Shell monitor started");
}

void ProgmanHost::StopMonitor() {
    if (!m_monitoring.load()) return;

    m_monitoring.store(false, std::memory_order_release);
    if (m_monitorThread.joinable()) {
        m_monitorThread.join();
    }

    REX_LOG_INFO("ProgmanHost", "Shell monitor stopped");
}

void ProgmanHost::MonitorShell() {
    using namespace std::chrono_literals;

    REX_LOG_DEBUG("ProgmanHost", "Monitor thread entered");

    while (m_monitoring.load(std::memory_order_acquire)) {
        HWND currentProgman = FindWindowW(L"Progman", nullptr);
        bool progmanChanged = (currentProgman != m_progman);

        bool windowAlive = (m_renderWindow != nullptr)
                        && IsWindow(m_renderWindow);

        if (!progmanChanged && windowAlive) {
            std::this_thread::sleep_for(2000ms);
            continue;
        }

        if (progmanChanged && currentProgman) {
            REX_LOG_INFO("ProgmanHost",
                "Progman changed (was {}, now {}). "
                "Shell likely restarted. Reacquiring...",
                fmt::ptr(m_progman), fmt::ptr(currentProgman));
        } else if (!windowAlive && m_progman && IsWindow(m_progman)) {
            REX_LOG_WARN("ProgmanHost",
                "Render window destroyed but Progman still alive. "
                "Reacquiring render window...");
        } else if (!currentProgman) {
            REX_LOG_WARN("ProgmanHost",
                "Progman not found. Waiting for shell to start...");
            std::this_thread::sleep_for(500ms);
            continue;
        }

        HWND oldRender = m_renderWindow;
        m_renderWindow = nullptr;
        m_progman      = nullptr;
        m_acquired     = false;

        if (oldRender && IsWindow(oldRender)) {
            DestroyWindow(oldRender);
        }

        std::this_thread::sleep_for(1000ms);

        auto result = const_cast<ProgmanHost*>(this)->Acquire();
        if (result.IsSuccess()) {
            REX_LOG_INFO("ProgmanHost",
                "Reacquired after shell restart (type={})",
                GetHostType());
            if (m_recoveryCallback) {
                m_recoveryCallback();
            }
        } else {
            REX_LOG_ERROR("ProgmanHost",
                "Failed to reacquire after shell restart: {}",
                result.Error().message);
        }

        std::this_thread::sleep_for(2000ms);
    }

    REX_LOG_DEBUG("ProgmanHost", "Monitor thread exiting");
}

} // namespace rex::platform::windows

``

## src/platform/windows/ProgmanHost.hpp

``cpp
#pragma once

#include "IDesktopHost.hpp"

#include <windows.h>
#include <atomic>
#include <functional>
#include <thread>

namespace rex::platform::windows {

class ProgmanHost : public IDesktopHost {
public:
    ProgmanHost();
    ~ProgmanHost() override;

    shared::Result<void> Acquire() override;
    void Release() override;

    HWND    GetRenderWindow() const override;
    int     GetWidth()  const override;
    int     GetHeight() const override;

    bool    IsValid()    const override;
    bool    IsAcquired() const override;

    std::string_view GetHostType() const override;

    void    StartMonitor(std::function<void()> onRecovery) override;
    void    StopMonitor() override;

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg,
                                     WPARAM wParam, LPARAM lParam);

    void MonitorShell();

    HWND m_progman      = nullptr;
    HWND m_renderWindow = nullptr;

    int m_width  = 0;
    int m_height = 0;

    bool m_acquired = false;
    HINSTANCE m_hInstance = nullptr;

    std::atomic<bool>              m_monitoring{false};
    std::thread                    m_monitorThread;
    std::function<void()>          m_recoveryCallback;
};

} // namespace rex::platform::windows

``

## src/platform/windows/MultiMonitorDesktopHost.cpp

``cpp
#include "MultiMonitorDesktopHost.hpp"

#include <shared/Logger.hpp>

namespace rex::platform::windows {

MultiMonitorDesktopHost::MultiMonitorDesktopHost()
    : m_manager(std::make_unique<WorkerWManager>()) {
}

MultiMonitorDesktopHost::~MultiMonitorDesktopHost() {
    Release();
}

shared::Result<void> MultiMonitorDesktopHost::Acquire() {
    if (m_manager->IsAcquired()) {
        return shared::Result<void>(shared::ErrorCode::AlreadyInitialized,
            "MultiMonitorDesktopHost already acquired");
    }

    REX_LOG_INFO("MultiMonitorDesktopHost",
        "Acquiring wallpaper WorkerW for multi-monitor hosting...");

    auto result = m_manager->Acquire();
    if (result.IsError()) {
        REX_LOG_ERROR("MultiMonitorDesktopHost",
            "WorkerWManager::Acquire failed: {}", result.Error().message);
        return result;
    }

    HWND workerW = m_manager->GetWorkerW();
    if (!workerW || !IsWindow(workerW)) {
        m_manager->Release();
        return shared::Result<void>(shared::ErrorCode::InvalidState,
            "WorkerW handle is null after successful acquisition");
    }

    HWND renderWindow = m_manager->GetRenderWindow();
    if (!renderWindow || !IsWindow(renderWindow)) {
        m_manager->Release();
        return shared::Result<void>(shared::ErrorCode::InvalidState,
            "Render window is not valid after WorkerW acquisition");
    }

    m_width  = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    m_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    REX_LOG_INFO("MultiMonitorDesktopHost",
        "Acquired: WorkerW {}, render window {}, virtual desktop {}x{}",
        fmt::ptr(workerW), fmt::ptr(renderWindow), m_width, m_height);

    return shared::Result<void>();
}

void MultiMonitorDesktopHost::Release() {
    if (!m_manager->IsAcquired()) return;

    REX_LOG_DEBUG("MultiMonitorDesktopHost", "Releasing desktop host...");
    m_manager->Release();
    m_width = 0;
    m_height = 0;
    REX_LOG_INFO("MultiMonitorDesktopHost", "Desktop host released");
}

HWND MultiMonitorDesktopHost::GetRenderWindow() const {
    return m_manager->GetRenderWindow();
}

int  MultiMonitorDesktopHost::GetWidth()  const { return m_width; }
int  MultiMonitorDesktopHost::GetHeight() const { return m_height; }

bool MultiMonitorDesktopHost::IsValid()    const { return m_manager->IsValid(); }
bool MultiMonitorDesktopHost::IsAcquired() const { return m_manager->IsAcquired(); }

std::string_view MultiMonitorDesktopHost::GetHostType() const { return "MultiMonitor"; }

void MultiMonitorDesktopHost::StartMonitor(std::function<void()> onRecovery) {
    m_manager->StartMonitor(std::move(onRecovery));
}

void MultiMonitorDesktopHost::StopMonitor() {
    m_manager->StopMonitor();
}

} // namespace rex::platform::windows
``

## src/platform/windows/MultiMonitorDesktopHost.hpp

``cpp
#pragma once

#include "IDesktopHost.hpp"
#include "WorkerWManager.hpp"

#include <memory>

namespace rex::platform::windows {

/// Desktop host for multi-monitor wallpapers. Acquires the same wallpaper
/// WorkerW as WorkerWHost (single render window spanning the virtual
/// desktop), but exposes the WorkerW so the engine can parent one
/// per-monitor child window inside the wallpaper layer.
class MultiMonitorDesktopHost : public IDesktopHost {
public:
    MultiMonitorDesktopHost();
    ~MultiMonitorDesktopHost() override;

    shared::Result<void> Acquire() override;
    void Release() override;

    HWND GetRenderWindow() const override;
    int  GetWidth()  const override;
    int  GetHeight() const override;

    bool IsValid()    const override;
    bool IsAcquired() const override;

    std::string_view GetHostType() const override;

    /// The wallpaper WorkerW: parent for per-monitor compositor windows.
    HWND GetWallpaperLayerWindow() const override {
        return m_manager->GetWorkerW();
    }

    void StartMonitor(std::function<void()> onRecovery) override;
    void StopMonitor() override;

    /// The wallpaper WorkerW (same as GetWallpaperLayerWindow).
    HWND GetWorkerW() const { return m_manager->GetWorkerW(); }

private:
    std::unique_ptr<WorkerWManager> m_manager;
    int m_width = 0;
    int m_height = 0;
};

} // namespace rex::platform::windows
``

## src/platform/windows/DesktopHostFactory.cpp

``cpp
#include "DesktopHostFactory.hpp"
#include "WorkerWHost.hpp"
#include "ProgmanHost.hpp"
#include "MultiMonitorDesktopHost.hpp"
#include <shared/Logger.hpp>

namespace rex::platform::windows {

std::unique_ptr<IDesktopHost> DesktopHostFactory::CreateBestAvailable() {
    REX_LOG_INFO("DesktopHost",
        "Searching for available desktop host strategy...");

    // Multi-monitor setups get per-monitor wallpaper compositing
    // automatically; no manual config needed.
    int monitorCount = GetSystemMetrics(SM_CMONITORS);
    if (monitorCount > 1) {
        REX_LOG_INFO("DesktopHost",
            "{} monitors detected, trying MultiMonitor...", monitorCount);
        auto multiHost = std::make_unique<MultiMonitorDesktopHost>();
        if (multiHost->Acquire().IsSuccess()) {
            REX_LOG_INFO("DesktopHost",
                "\xe2\x9c\x93 MultiMonitor strategy succeeded ({}x{})",
                multiHost->GetWidth(), multiHost->GetHeight());
            return multiHost;
        }
        REX_LOG_WARN("DesktopHost",
            "\xe2\x9c\x97 MultiMonitor failed, falling back to single-monitor hosts");
    }

    {
        REX_LOG_INFO("DesktopHost", "Trying WorkerW...");

        auto host = std::make_unique<WorkerWHost>();
        auto result = host->Acquire();

        if (result.IsSuccess()) {
            REX_LOG_INFO("DesktopHost",
                "\xe2\x9c\x93 WorkerW strategy succeeded ({}x{})",
                host->GetWidth(), host->GetHeight());
            return host;
        }

        REX_LOG_WARN("DesktopHost",
            "\xe2\x9c\x97 WorkerW failed: {}", result.Error().message);
    }

    {
        REX_LOG_INFO("DesktopHost", "Trying Progman...");

        auto host = std::make_unique<ProgmanHost>();
        auto result = host->Acquire();

        if (result.IsSuccess()) {
            REX_LOG_INFO("DesktopHost",
                "\xe2\x9c\x93 Progman strategy succeeded ({}x{})",
                host->GetWidth(), host->GetHeight());
            return host;
        }

        REX_LOG_WARN("DesktopHost",
            "\xe2\x9c\x97 Progman failed: {}", result.Error().message);
    }

    REX_LOG_ERROR("DesktopHost",
        "All desktop hosting strategies failed. "
        "Tried: WorkerW, Progman. "
        "This typically means Explorer.exe is not running or the "
        "desktop shell is in an unusual state. "
        "Check that Windows is logged in and the desktop is visible.");

    return nullptr;
}

std::unique_ptr<IDesktopHost> DesktopHostFactory::Create(
    const std::string& type)
{
    if (type == "WorkerW") {
        return std::make_unique<WorkerWHost>();
    }
    if (type == "Progman") {
        return std::make_unique<ProgmanHost>();
    }
    if (type == "MultiMonitor") {
        return std::make_unique<MultiMonitorDesktopHost>();
    }

    REX_LOG_ERROR("DesktopHost",
        "Unknown desktop host type: {}", type.c_str());
    return nullptr;
}

} // namespace rex::platform::windows

``

## src/platform/windows/DesktopHostFactory.hpp

``cpp
#pragma once

#include "IDesktopHost.hpp"
#include <memory>
#include <string>

namespace rex::platform::windows {

class DesktopHostFactory {
public:
    static std::unique_ptr<IDesktopHost> CreateBestAvailable();
    static std::unique_ptr<IDesktopHost> Create(const std::string& type);

private:
    DesktopHostFactory() = delete;
};

} // namespace rex::platform::windows

``

## src/platform/windows/DesktopWindowThread.cpp

``cpp
#include "DesktopWindowThread.hpp"

#include <shared/Logger.hpp>

#include <chrono>

namespace rex::platform::windows {

namespace {

// Completion state shared between Send() and the task it posts. Held via
// shared_ptr so the task stays valid even if Send() returns on timeout
// before the task runs.
struct SendState {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
};

} // namespace

DesktopWindowThread::~DesktopWindowThread() {
    Stop();
}

bool DesktopWindowThread::Start() {
    if (m_running.load(std::memory_order_acquire)) {
        return true;
    }

    m_stopRequested = false;
    m_running.store(true, std::memory_order_release);

    try {
        m_thread = std::thread(&DesktopWindowThread::ThreadProc, this);
    } catch (const std::system_error& ex) {
        REX_LOG_ERROR("DesktopWindowThread", "Failed to start thread: {}",
                      ex.what());
        m_running.store(false, std::memory_order_release);
        return false;
    }

    return m_thread.joinable();
}

void DesktopWindowThread::Stop() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopRequested = true;
    }
    m_cv.notify_all();

    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_running.store(false, std::memory_order_release);
}

void DesktopWindowThread::Post(Task task) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_tasks.push_back(std::move(task));
    }
    m_cv.notify_one();
}

bool DesktopWindowThread::Send(Task task, DWORD timeoutMs) {
    if (IsWindowThread()) {
        task();
        return true;
    }

    auto state = std::make_shared<SendState>();
    Post([state, task = std::move(task)]() {
        task();
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->done = true;
        }
        state->cv.notify_all();
    });

    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                            [&] { return state->done; })) {
        REX_LOG_WARN("DesktopWindowThread",
            "Send timed out after {} ms — task may still be queued", timeoutMs);
        return false;
    }
    return true;
}

void DesktopWindowThread::ThreadProc() {
    m_threadId.store(GetCurrentThreadId(), std::memory_order_release);
    REX_LOG_DEBUG("DesktopWindowThread", "Window thread entered (TID {})",
                  m_threadId.load());

    // Message-only anchor window: gives the thread a real message queue so
    // cross-thread SendMessage calls (from Explorer, or our own hosts) are
    // delivered and dispatched by PumpMessages below.
    constexpr wchar_t kClassName[] = L"REXDesktopWindowThread";
    WNDCLASSW wc = {};
    wc.lpfnWndProc   = MessageWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;

    if (!RegisterClassW(&wc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            REX_LOG_ERROR("DesktopWindowThread",
                "RegisterClassW failed for thread anchor (error: {})", err);
            m_running.store(false, std::memory_order_release);
            return;
        }
    }

    m_messageWindow = CreateWindowExW(
        0, kClassName, L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!m_messageWindow) {
        REX_LOG_ERROR("DesktopWindowThread",
            "Failed to create thread anchor window (error: {})",
            GetLastError());
        m_running.store(false, std::memory_order_release);
        return;
    }

    REX_LOG_DEBUG("DesktopWindowThread",
        "Thread anchor created: {}", fmt::ptr(m_messageWindow));

    while (m_running.load(std::memory_order_acquire)) {
        // 1. Dispatch any messages (Explorer SendMessage to our windows).
        PumpMessages();

        // 2. Run one pending task, then loop back so messages stay pumped
        //    between tasks (never run long window ops without pumping).
        Task task;
        bool stop;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            stop = m_stopRequested;
            if (!m_tasks.empty()) {
                task = std::move(m_tasks.front());
                m_tasks.pop_front();
            }
        }

        if (task) {
            task();
            continue;
        }

        if (stop) {
            break;
        }

        // 3. No work: wait for Post/Stop to wake us (bounded so PumpMessages
        //    still runs at least every 10 ms).
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait_for(lock, std::chrono::milliseconds(10));
    }

    // Drain any tasks still queued (e.g. posted by a timed-out Send that
    // later completed) — safe because they hold shared state / this.
    for (;;) {
        Task task;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_tasks.empty()) break;
            task = std::move(m_tasks.front());
            m_tasks.pop_front();
        }
        if (task) task();
    }

    if (m_messageWindow) {
        DestroyWindow(m_messageWindow);
        m_messageWindow = nullptr;
    }

    m_running.store(false, std::memory_order_release);
    REX_LOG_DEBUG("DesktopWindowThread", "Window thread exiting");
}

void DesktopWindowThread::PumpMessages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        // WM_QUIT is not expected (render windows no longer call
        // PostQuitMessage); Stop() governs thread lifetime.
    }
}

LRESULT CALLBACK DesktopWindowThread::MessageWndProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace rex::platform::windows

``

## src/platform/windows/DesktopWindowThread.hpp

``cpp
#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace rex::platform::windows {

// ─── DesktopWindowThread ───────────────────────────────────────────────────
//
// Owns every desktop-host HWND (create / reparent / resize / destroy) on a
// dedicated thread that runs a GetMessage pump. Without this, Explorer's
// cross-process SendMessage to a window we own is queued to the thread that
// created that window — and if that thread never dispatches messages (the
// old command/render threads), Explorer blocks forever (AppHangXProcB1).
//
// All window operations in ProgmanHost / WorkerWManager must be marshalled
// here via Send(); the caller blocks only briefly while the window thread
// executes and pumps. Read-only window queries (IsWindow / GetWindowLong /
// GetParent / GetWindow) are safe from any thread and stay off this thread.
//
// The thread is per-host-instance: created in Acquire(), stopped in Release().
// ServiceReacquire reuses the same thread — it is only torn down on a full
// host release.
class DesktopWindowThread {
public:
    using Task = std::function<void()>;

    DesktopWindowThread() = default;
    ~DesktopWindowThread();

    DesktopWindowThread(const DesktopWindowThread&) = delete;
    DesktopWindowThread& operator=(const DesktopWindowThread&) = delete;

    // Start the thread and its message-only anchor window. Safe to call
    // multiple times (no-op once running).
    bool Start();

    // Stop the thread: drains any remaining tasks, then joins. All pending
    // tasks complete before this returns.
    void Stop();

    // Queue a task for the window thread (non-blocking). The task runs with
    // a pumped message loop on the window thread.
    void Post(Task task);

    // Run a task on the window thread and wait for completion. Returns true
    // only when the task actually ran. Never touches the window thread from
    // the window thread itself (runs inline instead of deadlocking).
    bool Send(Task task, DWORD timeoutMs = 3000);

    bool IsRunning() const { return m_running.load(std::memory_order_acquire); }
    DWORD GetThreadId() const { return m_threadId; }

    // True if the calling thread is the window thread.
    bool IsWindowThread() const {
        return GetCurrentThreadId() == m_threadId;
    }

private:
    void ThreadProc();
    void PumpMessages();

    std::thread m_thread;
    std::atomic<DWORD> m_threadId{0};
    std::atomic<bool> m_running{false};
    HWND m_messageWindow = nullptr;   // owned by the window thread only

    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<Task> m_tasks;
    bool m_stopRequested = false;     // guarded by m_mutex

    static LRESULT CALLBACK MessageWndProc(HWND hwnd, UINT msg,
                                           WPARAM wParam, LPARAM lParam);
};

} // namespace rex::platform::windows

``

## src/platform/windows/IDesktopHost.hpp

``cpp
#pragma once

#include <windows.h>
#include <shared/Result.hpp>
#include <functional>
#include <string_view>

namespace rex::platform::windows {

class IDesktopHost {
public:
    virtual ~IDesktopHost() = default;

    virtual shared::Result<void> Acquire() = 0;
    virtual void Release() = 0;

    virtual HWND GetRenderWindow() const = 0;
    virtual int GetWidth()  const = 0;
    virtual int GetHeight() const = 0;

    virtual bool IsValid()    const = 0;
    virtual bool IsAcquired() const = 0;

    virtual std::string_view GetHostType() const = 0;

    /// Wallpaper layer window (parent of the render window), when the host
    /// exposes one. nullptr otherwise — per-monitor compositing is only
    /// available through a layer the engine can parent windows into.
    virtual HWND GetWallpaperLayerWindow() const { return nullptr; }

    virtual void StartMonitor(std::function<void()> /*onRecovery*/) {}
    virtual void StopMonitor() {}
};

} // namespace rex::platform::windows

``

## src/shared/PreciseSleep.hpp

``cpp
#pragma once

// ─── PreciseSleepUntil ────────────────────────────────────────────────────
//
// High-resolution sleep until an absolute steady_clock deadline, precise to
// ~0.5 ms. Backed by a CREATE_WAITABLE_TIMER_HIGH_RESOLUTION waitable timer
// (Windows 10 1809+), which is accurate regardless of the process timer
// quantum.
//
// Why this exists: since Windows 11 24H2 / 25H2 (build 26100+) the OS no
// longer honors timeBeginPeriod(1) — the system timer stays at the default
// 15.6 ms quantum, so Sleep()/sleep_until() round up to the next quantum
// (~31 ms for a 16.7 ms sleep → 32 fps instead of 60). The high-resolution
// waitable timer is the supported way to get fine-grained waits on those
// builds.
//
// Falls back to sleep_until() + a spin of the tail for very short sleeps
// (or when the flag is unsupported).

#include <chrono>
#include <thread>

#include <windows.h>

namespace rex::shared {

template <typename Clock, typename Duration>
inline void PreciseSleepUntil(std::chrono::time_point<Clock, Duration> deadline) {
    static_assert(std::is_same<Clock, std::chrono::steady_clock>::value,
                  "PreciseSleepUntil requires a steady_clock deadline");
    using namespace std::chrono;
    const auto now = steady_clock::now();
    if (deadline <= now) {
        return;
    }

    const auto remainingUs =
        duration_cast<microseconds>(deadline - now).count();

    // Only bother with the waitable timer for meaningful sleeps; spin the
    // tail for anything shorter than ~2 ms (SetWaitableTimer has fixed
    // overhead too).
    if (remainingUs > 2000) {
        static thread_local HANDLE s_timer = [] {
            return CreateWaitableTimerExW(
                nullptr, nullptr,
                CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        }();
        if (s_timer) {
            LARGE_INTEGER due;
            due.QuadPart = -10 * remainingUs;  // relative, 100 ns units
            if (SetWaitableTimer(s_timer, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(s_timer, INFINITE);
                return;
            }
        }
    }

    // Fallback: coarse sleep_until (quantum-limited wake), then spin the
    // last ~1.5 ms so the deadline is still honored.
    const auto coarse = deadline - microseconds(1500);
    if (coarse > now) {
        std::this_thread::sleep_until(coarse);
    }
    while (steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
}

} // namespace rex::shared

``

## include/rex/wallpaper/IWallpaperEngine.hpp

``cpp
#pragma once

#include <shared/Result.hpp>
#include <shared/UUID.hpp>

#include <string>
#include <vector>
#include <memory>
#include <optional>

namespace rex::settings { class ISettingsService; }

namespace rex::wallpaper {

// ─── Forward Declarations ──────────────────────────────────────────────────

struct SceneInfo;
struct MonitorInfo;
struct WallpaperConfig;

// ─── Enums ──────────────────────────────────────────────────────────────────

enum class EngineState {
    Uninitialized,
    Initializing,
    Initialized,
    Running,
    Paused,
    Stopped,
    Error,
    ShuttingDown
};

enum class FillMode {
    Fill,    // Scale to fill screen, crop excess (default)
    Fit,     // Scale to fit within screen, letterbox
    Stretch, // Stretch to fill screen (ignore aspect ratio)
    Center,  // Center at native resolution
    Tile     // Tile at native resolution (repeat)
};

// ─── IRuntime ──────────────────────────────────────────────────────────────

/// Base interface for all REX runtimes.
/// Every runtime (Wallpaper, WindowFX) implements this.
class IRuntime {
public:
    virtual ~IRuntime() = default;

    virtual shared::Result<void> Initialize(const WallpaperConfig& config) = 0;
    virtual shared::Result<void> Shutdown() = 0;
    virtual EngineState GetState() const = 0;
    virtual shared::UUID GetSessionId() const = 0;
};

// ─── SceneInfo ─────────────────────────────────────────────────────────────

struct SceneInfo {
    std::string id;
    std::string name;
    std::string sourcePath;
    std::string type = "static"; // "static", "video", "web", "interactive"
    bool    loop     = true;
    bool    muted    = false;
    float   volume   = 1.0f;
    float   speed    = 1.0f;
    float   brightness = 1.0f;
    float   contrast   = 1.0f;
    float   saturation = 1.0f;
    FillMode fillMode = FillMode::Fill;
};

// ─── MonitorInfo ───────────────────────────────────────────────────────────

struct MonitorInfo {
    std::string id;
    std::string name;
    int     x          = 0;
    int     y          = 0;
    int     width      = 0;
    int     height     = 0;
    bool    isPrimary  = false;
    float   dpiScale   = 1.0f;
    bool    connected  = true;
    // Stable identity across disconnects (EDID hash; 0 = not available).
    uint64_t edidHash  = 0;
};

// ─── WallpaperConfig ───────────────────────────────────────────────────────

struct WallpaperConfig {
    bool  enableHardwareAcceleration = true;
    int   maxFps       = 60;
    int   targetMonitor = 0;
    bool  enableAudioReactive = false;
    float audioSensitivity    = 0.5f;
    // Desktop host strategy. Empty = auto (best available, single-monitor
    // WorkerW/Progman). "MultiMonitor" = per-monitor wallpaper compositing.
    std::string desktopHost;
    // Auto-pause the wallpaper while a full-screen application has the
    // foreground, and resume when it closes. Detection is monitor-aware:
    // a window covering >=95% of its monitor counts as full-screen.
    bool pauseOnFullscreen = true;
    // --force-sdr diagnostic: override hdrOutput to 0 in the shader so SDR
    // video always goes through the simple BT.709 + sRGB path.
    bool forceSdr = false;
};

// ─── EngineTelemetry ────────────────────────────────────────────────────────
//
// Live counters for diagnosing memory/CPU retention in the native runtime.
// All values are snapshots of atomics/counters; nothing here blocks.

struct EngineTelemetry {
    // Decoder / playback pipeline
    uint64_t framesDecoded   = 0;
    uint64_t framesConsumed  = 0;
    uint64_t framesDropped   = 0;
    double   decoderFps      = 0.0;
    double   renderFps       = 0.0;
    int      videoWidth      = 0;
    int      videoHeight     = 0;
    uint64_t ringSlotCount   = 0;
    uint64_t ringBytes       = 0;  // sum of ring slot pixel capacities

    // GPU (D3D12)
    uint64_t textureBytes    = 0;  // video texture committed bytes
    uint64_t uploadBytes     = 0;  // upload buffer bytes
    uint64_t textureRecreations = 0;  // texture re-creations (size changes)
    uint64_t uploads         = 0;  // total texture uploads performed
    uint64_t allocatorsPerFrame = 0; // command allocators created per upload

    // Scenes
    uint64_t scenesLoaded    = 0;
    uint64_t scenesReleased  = 0;

    // Adaptive resolution (selected stream size)
    int  decodeWidth  = 0;
    int  decodeHeight = 0;

    // HDR output
    bool  hdrActive      = false;
    float displayMaxNits = 0.0f;

    // Process
    double   privateBytesMB  = 0.0;

    // Presents (P16): actual Present() calls — should track content rate
    // (min(decoderFps, maxFps)), NOT the display refresh rate.
    uint64_t presentCount = 0;

    // P13 per-stage decode-pipeline percentiles (ms, last 128 frames).
    double decodeMsP50 = 0.0, decodeMsP95 = 0.0, decodeMsP99 = 0.0;
    double vpMsP50     = 0.0, vpMsP95     = 0.0, vpMsP99     = 0.0;

    // GPU memory (adapter of the D3D12 renderer)
    uint64_t gpuDedicatedBytes = 0;  // LOCAL segment current usage
    uint64_t gpuSharedBytes    = 0;  // NON_LOCAL segment current usage
    uint64_t gpuBudgetBytes    = 0;  // LOCAL segment budget

    // Adapter topology (0 = unknown)
    bool     decoderRendererSameAdapter  = false;
    bool     rendererDisplaySameAdapter  = false;
    uint64_t decoderAdapterLuid  = 0;
    uint64_t rendererAdapterLuid = 0;
};

// ─── IWallpaperEngine ──────────────────────────────────────────────────────

class IWallpaperEngine : public IRuntime {
public:
    ~IWallpaperEngine() override = default;

    // ─── Lifecycle (inherited from IRuntime) ──────────────────────────────
    // Initialize(), Shutdown(), GetState(), GetSessionId()

    // ─── Scene ────────────────────────────────────────────────────────────
    virtual shared::Result<void> LoadScene(const SceneInfo& scene) = 0;
    virtual shared::Result<void> UnloadScene() = 0;
    virtual SceneInfo GetCurrentScene() const = 0;

    // ─── Playback ─────────────────────────────────────────────────────────
    virtual shared::Result<void> Play() = 0;
    virtual shared::Result<void> Pause() = 0;
    virtual shared::Result<void> Stop() = 0;
    virtual bool   IsPlaying() const = 0;
    virtual double GetPosition() const = 0;
    virtual double GetDuration() const = 0;

    // ─── Monitor ──────────────────────────────────────────────────────────
    virtual shared::Result<void> AssignMonitor(const std::string& monitorId) = 0;
    virtual shared::Result<void> ClearMonitor() = 0;
    virtual MonitorInfo GetCurrentMonitor() const = 0;
    virtual std::vector<MonitorInfo> EnumerateMonitors() const = 0;

    // ─── Per-monitor wallpapers ─────────────────────────────────────────
    /// Pin a wallpaper to a monitor (EDID-keyed, persisted via the
    /// settings service). Empty path clears the override.
    virtual shared::Result<void> SetPerMonitorWallpaper(
        const std::string& monitorId, const std::string& path) = 0;
    /// Provide the settings service used to persist/restore assignments.
    virtual void SetSettingsService(
        std::shared_ptr<rex::settings::ISettingsService> settings) = 0;

    // ─── Settings ─────────────────────────────────────────────────────────
    virtual shared::Result<void> SetFillMode(FillMode mode) = 0;
    virtual shared::Result<void> SetBrightness(float value) = 0;
    virtual shared::Result<void> SetContrast(float value) = 0;
    virtual shared::Result<void> SetSaturation(float value) = 0;
    virtual shared::Result<void> SetVolume(float value) = 0;
    virtual shared::Result<void> SetMute(bool mute) = 0;
    virtual shared::Result<void> SetLoop(bool loop) = 0;
    virtual shared::Result<void> SetSpeed(float speed) = 0;

    // ─── Renderer Adapter ────────────────────────────────────────────────
    /// Applies a runtime property ("fillMode", "volume", "speed", "loop",
    /// ...) to the active renderer adapter. Returns an error when the
    /// property is unknown or unsupported by the active renderer, so
    /// callers never silently no-op.
    virtual shared::Result<void> SetProperty(const std::string& property, const std::string& value) = 0;

    /// Properties the active renderer can apply (empty when no scene loaded).
    virtual std::vector<std::string> GetSupportedProperties() const = 0;

    /// Active renderer type name ("static", "video", ... or "none").
    virtual std::string GetRendererType() const = 0;

    // ─── Audio ───────────────────────────────────────────────────────────
    virtual shared::Result<void> EnableAudioReactive(bool enable) = 0;
    virtual shared::Result<void> SetAudioSensitivity(float sensitivity) = 0;

    // ─── Debug ────────────────────────────────────────────────────────────
    virtual void DumpStats() const = 0;

    // ─── Frame surface (preview / cross-process sharing) ─────────────────
    /// Snapshot of the currently published video frame surface. `available`
    /// is false when no video is open. Handle values are PROCESS-LOCAL
    /// NT handles; a host process must have them duplicated into its own
    /// address space (see the runtime's SET_CLIENT_PID / GET_FRAME_SURFACE
    /// protocol) before calling ID3D12Device::OpenSharedHandle.
    struct FrameSurface {
        bool available = false;
        uint64_t fenceHandle = 0;   // NT handle of the shared D3D12 fence
        uint64_t adapterLuid = 0;   // adapter LUID (LowPart | HighPart<<32)
        uint32_t slotCount = 0;     // pool slots (stable per video open)
        uint32_t currentSlot = 0;   // slot of the last published frame
        uint64_t currentFenceValue = 0;  // fence value to wait for
        uint32_t width = 0;
        uint32_t height = 0;
        uint64_t frameIndex = 0;    // monotonic decode counter (change detector)
        uint64_t generation = 0;    // bumped per video open/close (re-dup key)
    };
    /// Latest published frame surface (safe from any thread; cheap).
    virtual FrameSurface GetFrameSurface() const = 0;
    /// Process-local NT handle of a pool slot texture (0..slotCount-1).
    virtual uint64_t GetFrameSlotHandle(uint32_t slot) const = 0;

    // ─── Telemetry ────────────────────────────────────────────────────────
    /// Snapshot of live allocation counters (decoder/ring/GPU/scenes).
    /// Cheap and non-blocking; safe to call from any thread.
    virtual void GetTelemetry(EngineTelemetry& out) const = 0;
};

// ─── Factory ───────────────────────────────────────────────────────────────

std::unique_ptr<IWallpaperEngine> CreateWallpaperEngine();

} // namespace rex::wallpaper

``

## include/rex/wallpaper/IRendererAdapter.hpp

``cpp
#pragma once

#include "rex/wallpaper/IWallpaperEngine.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rex::wallpaper {

// ─── IRendererAdapter ──────────────────────────────────────────────────────
//
// Bridges the generic engine property surface to a concrete renderer.
// Each renderer declares the properties it can actually apply, so callers
// receive an explicit "unsupported" error instead of a silent no-op.
//
//   LoadScene()   → creates the adapter for the scene's type
//   SetProperty() → rejects unknown / unsupported properties
//   GetSupportedProperties() → introspection for UI gating

class IRendererAdapter {
public:
    virtual ~IRendererAdapter() = default;

    /// Renderer type name: "static", "video", "web", "shader", "scene",
    /// "audio_visualizer", or "unsupported".
    virtual std::string_view GetType() const = 0;

    /// Properties this renderer can apply (see Engine::SetProperty).
    virtual std::vector<std::string> GetSupportedProperties() const = 0;

    virtual bool Supports(std::string_view property) const = 0;
};

// ─── Factory & Helpers ─────────────────────────────────────────────────────

std::unique_ptr<IRendererAdapter> CreateRendererAdapter(std::string_view sceneType);

std::optional<FillMode> ParseFillMode(std::string_view value);
std::string_view ToString(FillMode mode);

} // namespace rex::wallpaper

``

## src/wallpaper/loader/ImageLoader.cpp

``cpp
#include "ImageLoader.hpp"

#include <shared/Logger.hpp>

#include <algorithm>
#include <cctype>
#include <string>

namespace rex::wallpaper::loader {

namespace {

    std::string Extension(std::string_view path) {
        auto dot = path.rfind('.');
        if (dot == std::string_view::npos) return "";
        std::string ext(path.substr(dot));
        // lowercase
        std::transform(ext.begin(), ext.end(), ext.begin(),
            [](unsigned char c) { return std::tolower(c); });
        return ext;
    }

} // anonymous namespace

// ─── Construction / Destruction ─────────────────────────────────────────────

ImageLoader::ImageLoader() = default;

ImageLoader::~ImageLoader() {
    StopHotReload();
}

// ─── Loading ────────────────────────────────────────────────────────────────

shared::Result<const graphics::CachedTexture*> ImageLoader::Load(
    std::string_view path)
{
    if (path.empty()) {
        return shared::Result<const graphics::CachedTexture*>(
            shared::ErrorCode::InvalidArgument,
            "Empty path");
    }

    if (!IsFormatSupported(path)) {
        return shared::Result<const graphics::CachedTexture*>(
            shared::ErrorCode::UnsupportedFormat,
            "Unsupported image format: " + std::string(path));
    }

    return m_cache.GetOrLoad(path);
}

shared::Result<const graphics::CachedTexture*> ImageLoader::Reload(
    std::string_view path)
{
    return m_cache.Reload(path);
}

void ImageLoader::Unload(std::string_view path) {
    m_cache.Evict(path);
}

void ImageLoader::Clear() {
    m_cache.Clear();
}

// ─── Queries ────────────────────────────────────────────────────────────────

bool ImageLoader::IsLoaded(std::string_view path) const {
    return m_cache.Contains(path);
}

const graphics::CachedTexture* ImageLoader::Get(
    std::string_view path) const
{
    return m_cache.Get(path);
}

ImageFormat ImageLoader::DetectFormat(std::string_view path) {
    std::string ext = Extension(path);

    if (ext == ".png")  return ImageFormat::PNG;
    if (ext == ".jpg" || ext == ".jpeg") return ImageFormat::JPEG;
    if (ext == ".bmp")  return ImageFormat::BMP;
    if (ext == ".tiff" || ext == ".tif") return ImageFormat::TIFF;
    if (ext == ".webp") return ImageFormat::WEBP;
    if (ext == ".gif")  return ImageFormat::GIF;
    if (ext == ".ico")  return ImageFormat::ICO;

    return ImageFormat::Unknown;
}

bool ImageLoader::IsFormatSupported(std::string_view path) {
    return DetectFormat(path) != ImageFormat::Unknown;
}

size_t ImageLoader::CacheSize() const {
    return m_cache.Size();
}

// ─── Hot-reload ─────────────────────────────────────────────────────────────

void ImageLoader::StartHotReload(
    std::chrono::milliseconds intervalMs,
    ChangeCallback onChange)
{
    if (m_hotReloadRunning.load()) return;

    m_changeCallback = std::move(onChange);
    m_pollInterval   = intervalMs;
    m_hotReloadRunning.store(true, std::memory_order_release);

    m_hotReloadThread = std::thread([this]() {
        REX_LOG_INFO("ImageLoader", "Hot-reload started ({}ms)",
                     m_pollInterval.count());

        while (m_hotReloadRunning.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(m_pollInterval);

            auto changed = m_cache.PollChangedFiles();
            for (const auto& path : changed) {
                auto result = m_cache.Reload(path);
                if (result.IsSuccess() && m_changeCallback) {
                    REX_LOG_INFO("ImageLoader",
                        "Hot-reload detected change: {}", path);
                    m_changeCallback(path);
                }
            }
        }

        REX_LOG_INFO("ImageLoader", "Hot-reload stopped");
    });
}

void ImageLoader::StopHotReload() {
    if (!m_hotReloadRunning.load()) return;

    m_hotReloadRunning.store(false, std::memory_order_release);
    if (m_hotReloadThread.joinable()) {
        m_hotReloadThread.join();
    }
}

std::vector<std::string> ImageLoader::PollChanges() {
    auto changed = m_cache.PollChangedFiles();
    for (const auto& path : changed) {
        m_cache.Reload(path);
    }
    return changed;
}

} // namespace rex::wallpaper::loader

``

## src/wallpaper/loader/ImageLoader.hpp

``cpp
#pragma once

#include "graphics/resource/TextureCache.hpp"

#include <shared/Result.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace rex::wallpaper::loader {

// ─── SupportedFormats ──────────────────────────────────────────────────────
//
// Image formats decodable by WIC (Windows Imaging Component).
// Codec availability depends on the Windows version and installed codecs.

enum class ImageFormat {
    Unknown,
    PNG,
    JPEG,
    BMP,
    TIFF,
    WEBP,  // requires Windows 10 1809+
    GIF,
    ICO,
};

// ─── ImageLoader ────────────────────────────────────────────────────────────
//
// Loads and caches wallpaper images. Provides:
//
//   - Async loading with callback
//   - Format detection / validation
//   - Hot-reload via periodic polling (file modification time)
//   - Texture cache integration
//
// ## Hot-reload
//
// Call StartHotReload() with a polling interval. The loader will check
// all cached images for file changes. When a change is detected, the
// image is reloaded and the changeCallback is invoked.
//
// ## Thread safety
//
// The underlying TextureCache is thread-safe. ImageLoader itself must
// be used from a single thread for Start/Stop/Reload control, but
// GetOrLoad can be called from any thread.

class ImageLoader {
public:
    /// Called when a hot-reload detects a changed file.
    /// The path is the file that changed.
    using ChangeCallback = std::function<void(std::string_view path)>;

    ImageLoader();
    ~ImageLoader();

    ImageLoader(const ImageLoader&) = delete;
    ImageLoader& operator=(const ImageLoader&) = delete;

    // ── Loading ─────────────────────────────────────────────────────────

    /// Load an image, caching if successful. Returns the decoded pixel
    /// data. Supported formats: PNG, JPEG, BMP, TIFF, WEBP, GIF, ICO.
    shared::Result<const graphics::CachedTexture*> Load(
        std::string_view path);

    /// Reload an image from disk (for hot-reload or explicit refresh).
    shared::Result<const graphics::CachedTexture*> Reload(
        std::string_view path);

    /// Evict an image from the cache.
    void Unload(std::string_view path);

    /// Evict all cached images.
    void Clear();

    // ── Queries ─────────────────────────────────────────────────────────

    /// Check whether an image is cached.
    bool IsLoaded(std::string_view path) const;

    /// Get cached texture data, or nullptr.
    const graphics::CachedTexture* Get(std::string_view path) const;

    /// Detect the image format from file extension.
    static ImageFormat DetectFormat(std::string_view path);

    /// Whether the format is supported.
    static bool IsFormatSupported(std::string_view path);

    /// Number of cached images.
    size_t CacheSize() const;

    // ── Hot-reload ──────────────────────────────────────────────────────

    /// Start hot-reload polling. Checks all cached files for changes
    /// every `intervalMs` milliseconds. When a change is detected,
    /// reloads the file and invokes `onChange`.
    void StartHotReload(std::chrono::milliseconds intervalMs,
                        ChangeCallback onChange);

    /// Stop hot-reload polling.
    void StopHotReload();

    /// Manually poll for changed files. Reloads changed files and
    /// returns their paths. Does not invoke the change callback.
    std::vector<std::string> PollChanges();

private:
    graphics::TextureCache m_cache;

    // Hot-reload
    std::atomic<bool> m_hotReloadRunning{false};
    std::thread m_hotReloadThread;
    ChangeCallback m_changeCallback;
    std::chrono::milliseconds m_pollInterval{2000};
};

} // namespace rex::wallpaper::loader

``

## src/wallpaper/playlist/PlaylistManager.cpp

``cpp
#include "PlaylistManager.hpp"
#include "wallpaper/runtime/RuntimeSettings.hpp"
#include "wallpaper/runtime/Json.hpp"
#include <rex/wallpaper/IRendererAdapter.hpp>
#include <shared/Logger.hpp>

#include <algorithm>

namespace rex::wallpaper {

using json = rex::runtime::Json;

PlaylistManager::PlaylistManager(std::shared_ptr<RuntimeSettings> settings)
    : m_settings(settings) {
    LoadAll();
}

std::vector<Playlist> PlaylistManager::GetAll() const {
    std::lock_guard lock(m_mutex);
    return m_playlists;
}

bool PlaylistManager::GetById(const std::string& id, Playlist& out) const {
    std::lock_guard lock(m_mutex);
    for (const auto& p : m_playlists) {
        if (p.id == id) { out = p; return true; }
    }
    return false;
}

bool PlaylistManager::Create(const Playlist& playlist) {
    std::lock_guard lock(m_mutex);
    for (const auto& p : m_playlists) {
        if (p.id == playlist.id) return false;
    }
    m_playlists.push_back(playlist);
    SaveAll();
    return true;
}

bool PlaylistManager::Update(const Playlist& playlist) {
    std::lock_guard lock(m_mutex);
    for (auto& p : m_playlists) {
        if (p.id == playlist.id) {
            p = playlist;
            SaveAll();
            return true;
        }
    }
    return false;
}

bool PlaylistManager::Delete(const std::string& id) {
    std::lock_guard lock(m_mutex);
    auto it = std::find_if(m_playlists.begin(), m_playlists.end(),
        [&](const Playlist& p) { return p.id == id; });
    if (it == m_playlists.end()) return false;
    m_playlists.erase(it);
    if (m_settings) m_settings->RemoveKey("playlist." + id);
    SaveAll();
    return true;
}

bool PlaylistManager::AddItem(const std::string& playlistId, const PlaylistItem& item) {
    std::lock_guard lock(m_mutex);
    for (auto& p : m_playlists) {
        if (p.id == playlistId) {
            p.items.push_back(item);
            SaveAll();
            return true;
        }
    }
    return false;
}

bool PlaylistManager::RemoveItem(const std::string& playlistId, size_t index) {
    std::lock_guard lock(m_mutex);
    for (auto& p : m_playlists) {
        if (p.id == playlistId && index < p.items.size()) {
            p.items.erase(p.items.begin() + index);
            SaveAll();
            return true;
        }
    }
    return false;
}

bool PlaylistManager::Reorder(const std::string& playlistId, const std::vector<size_t>& order) {
    std::lock_guard lock(m_mutex);
    for (auto& p : m_playlists) {
        if (p.id == playlistId) {
            if (order.size() != p.items.size()) return false;
            std::vector<PlaylistItem> reordered;
            for (size_t idx : order) {
                if (idx >= p.items.size()) return false;
                reordered.push_back(p.items[idx]);
            }
            p.items = std::move(reordered);
            SaveAll();
            return true;
        }
    }
    return false;
}

void PlaylistManager::LoadAll() {
    std::lock_guard lock(m_mutex);
    m_playlists.clear();
    if (!m_settings) return;

    const std::string idsStr = m_settings->GetString("playlists.list");
    if (idsStr.empty()) return;

    auto idsJson = json::Parse(idsStr);
    if (!idsJson || !idsJson->IsArray()) return;

    for (const auto& idJson : idsJson->AsArray()) {
        std::string id = idJson.AsString();
        const std::string val = m_settings->GetString("playlist." + id);
        if (!val.empty()) {
            Playlist p;
            if (Deserialize(val, p)) {
                m_playlists.push_back(p);
            }
        }
    }
}

void PlaylistManager::SaveAll() {
    if (!m_settings) return;
    std::vector<std::string> ids;
    for (const auto& p : m_playlists) {
        const std::string key = "playlist." + p.id;
        m_settings->SetString(key, Serialize(p));
        ids.push_back(p.id);
    }
    std::string list = "[";
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i) list += ",";
        list += "\"" + ids[i] + "\"";
    }
    list += "]";
    m_settings->SetString("playlists.list", list);
}

std::string PlaylistManager::Serialize(const Playlist& p) const {
    json::Json::Object o;
    o["id"] = p.id;
    o["name"] = p.name;
    o["shuffle"] = p.shuffle;
    o["loopAll"] = p.loopAll;
    o["intervalSeconds"] = p.intervalSeconds;
    o["lastIndex"] = static_cast<double>(p.lastIndex);
    o["updatedAt"] = p.updatedAt;

    json::Json::Array itemsArr;
    for (const auto& item : p.items) {
        json::Json::Object io;
        io["type"] = item.type;
        io["path"] = item.path;
        io["fillMode"] = std::string(ToString(item.fillMode));
        io["volume"] = item.volume;
        io["speed"] = item.speed;
        io["loop"] = item.loop;
        itemsArr.push_back(json::Json(io));
    }
    o["items"] = json::Json(itemsArr);
    return json::Json(o).Dump();
}

bool PlaylistManager::Deserialize(const std::string& jsonStr, Playlist& out) const {
    auto parsed = json::Parse(jsonStr);
    if (!parsed) return false;
    const auto& o = parsed->AsObject();
    out.id = o.at("id").AsString();
    out.name = o.at("name").AsString();
    out.shuffle = o.at("shuffle").AsBool(false);
    out.loopAll = o.at("loopAll").AsBool(true);
    out.intervalSeconds = o.at("intervalSeconds").AsFloat(0.0);
    // Missing fields (playlists written before resume support) default to
    // no resume point — safe for older persisted data.
    out.lastIndex = static_cast<size_t>(o.at("lastIndex").AsInt(0));
    out.updatedAt = o.at("updatedAt").AsString();

    out.items.clear();
    const auto& items = o.at("items");
    if (items.IsArray()) {
        for (const auto& itemJson : items.AsArray()) {
            const auto& io = itemJson.AsObject();
            PlaylistItem item;
            item.type = io.at("type").AsString();
            item.path = io.at("path").AsString();
            item.fillMode = ParseFillMode(io.at("fillMode").AsString("fill"))
                                .value_or(FillMode::Fill);
            item.volume = io.at("volume").AsFloat(1.0f);
            item.speed = io.at("speed").AsFloat(1.0f);
            item.loop = io.at("loop").AsBool(true);
            out.items.push_back(item);
        }
    }
    return true;
}

} // namespace rex::wallpaper
``

## src/wallpaper/playlist/PlaylistManager.hpp

``cpp
#pragma once
#include "Playlist.hpp"
#include "wallpaper/runtime/RuntimeSettings.hpp"
#include <memory>
#include <string>
#include <vector>
#include <mutex>

namespace rex::wallpaper {

class PlaylistManager {
public:
    explicit PlaylistManager(std::shared_ptr<RuntimeSettings> settings);

    std::vector<Playlist> GetAll() const;
    bool GetById(const std::string& id, Playlist& out) const;

    bool Create(const Playlist& playlist);
    bool Update(const Playlist& playlist);
    bool Delete(const std::string& id);

    bool AddItem(const std::string& playlistId, const PlaylistItem& item);
    bool RemoveItem(const std::string& playlistId, size_t index);
    bool Reorder(const std::string& playlistId, const std::vector<size_t>& order);

    /// Wire-friendly JSON dump of a playlist (used by PLAYLIST_GET).
    std::string Serialize(const Playlist& p) const;

    void LoadAll();
    void SaveAll();

private:
    mutable std::mutex m_mutex;
    std::shared_ptr<RuntimeSettings> m_settings;
    std::vector<Playlist> m_playlists;

    bool Deserialize(const std::string& json, Playlist& out) const;
};

} // namespace rex::wallpaper
``

## src/wallpaper/playlist/PlaylistPlayer.cpp

``cpp
#include "PlaylistPlayer.hpp"
#include "PlaylistManager.hpp"
#include <rex/wallpaper/IWallpaperEngine.hpp>
#include <shared/Logger.hpp>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <random>

namespace rex::wallpaper {

namespace {
std::string NowIso8601() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_s(&tm, &t);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return std::string(buf);
}
} // namespace

PlaylistPlayer::PlaylistPlayer(std::shared_ptr<IWallpaperEngine> engine,
                               std::shared_ptr<PlaylistManager> playlistManager)
    : m_engine(engine), m_playlistManager(std::move(playlistManager)) {}

PlaylistPlayer::~PlaylistPlayer() { Stop(); }

bool PlaylistPlayer::Start(const Playlist& playlist, size_t startIndex,
                           bool resume) {
    if (playlist.items.empty()) return false;
    Stop();
    m_playlist = playlist;
    if (resume && m_playlist.lastIndex < m_playlist.items.size()) {
        // Continue from the last played item of a previous session.
        m_currentIndex = m_playlist.lastIndex;
    } else {
        m_currentIndex = startIndex < m_playlist.items.size() ? startIndex : 0;
    }
    m_stopRequested = false;
    m_running = true;
    m_thread = std::thread(&PlaylistPlayer::PlayerLoop, this);
    if (m_eventSink) {
        const auto& item = m_playlist.items[m_currentIndex];
        m_eventSink("started", m_currentIndex, item.path, item.type);
    }
    return true;
}

void PlaylistPlayer::Stop() {
    const bool wasRunning = m_running.load();
    m_stopRequested = true;
    m_running = false;
    if (m_thread.joinable()) m_thread.join();
    if (wasRunning && m_eventSink) {
        m_eventSink("stopped", m_currentIndex, "", "");
    }
}

void PlaylistPlayer::Next() {
    std::lock_guard lock(m_mutex);
    if (!m_running) return;
    size_t idx = NextIndex(m_currentIndex, true);
    LoadAndPlay(idx);
}

void PlaylistPlayer::Previous() {
    std::lock_guard lock(m_mutex);
    if (!m_running) return;
    size_t idx = NextIndex(m_currentIndex, false);
    LoadAndPlay(idx);
}

Playlist PlaylistPlayer::GetCurrentPlaylist() const {
    std::lock_guard lock(m_mutex);
    return m_playlist;
}

void PlaylistPlayer::PlayerLoop() {
    LoadAndPlay(m_currentIndex);
    while (m_running && !m_stopRequested) {
        double duration = GetItemDuration(m_playlist.items[m_currentIndex]);
        // Sleep in small intervals to allow fast stop.
        double elapsed = 0.0;
        while (elapsed < duration && m_running && !m_stopRequested) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            elapsed += 0.1;
        }
        if (m_stopRequested) break;

        // If the item loops, stay on it (but still check stop).
        if (m_playlist.items[m_currentIndex].loop && m_playlist.loopAll) {
            continue;
        }
        std::lock_guard lock(m_mutex);
        size_t idx = NextIndex(m_currentIndex, true);
        LoadAndPlay(idx);
    }
}

void PlaylistPlayer::LoadAndPlay(size_t index) {
    if (index >= m_playlist.items.size()) return;
    m_currentIndex = index;
    const auto& item = m_playlist.items[index];

    SceneInfo scene;
    scene.sourcePath = item.path;
    scene.type = item.type;
    scene.fillMode = item.fillMode;
    scene.volume = item.volume;
    scene.speed = item.speed;
    scene.loop = item.loop;

    auto result = m_engine->LoadScene(scene);
    if (result.IsError()) {
        REX_LOG_WARN("PlaylistPlayer", "Failed to load scene: {}", result.Error().message);
        if (m_eventSink) m_eventSink("item-error", index, item.path, item.type);
        return;
    }
    m_engine->Play();
    // Remember where we are so a restart resumes at this item.
    PersistCurrentPosition();
    if (m_eventSink) m_eventSink("item", index, item.path, item.type);
}

void PlaylistPlayer::PersistCurrentPosition() {
    if (!m_playlistManager) return;
    m_playlist.lastIndex = m_currentIndex;
    m_playlist.updatedAt = NowIso8601();
    m_playlistManager->Update(m_playlist);
}

double PlaylistPlayer::GetItemDuration(const PlaylistItem& item) {
    if (m_playlist.intervalSeconds > 0.0) return m_playlist.intervalSeconds;
    if (item.type == "video") {
        double d = m_engine->GetDuration();
        if (d > 0.0) return d / std::max(0.25f, item.speed);
    }
    // Static/shader: use a default 30s or the interval override.
    return 30.0;
}

size_t PlaylistPlayer::NextIndex(size_t current, bool forward) {
    if (m_playlist.items.empty()) return 0;
    if (m_playlist.shuffle) {
        static std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<size_t> dist(0, m_playlist.items.size() - 1);
        return dist(rng);
    }
    size_t count = m_playlist.items.size();
    size_t next;
    if (forward) {
        next = (current + 1) % count;
        if (next == 0 && !m_playlist.loopAll) {
            // Reached the end and no loop → stop (stay on the last item).
            m_running = false;
            if (m_eventSink) m_eventSink("finished", current, "", "");
            return current;
        }
    } else {
        next = (current == 0) ? count - 1 : current - 1;
    }
    return next;
}

} // namespace rex::wallpaper
``

## src/wallpaper/playlist/PlaylistPlayer.hpp

``cpp
#pragma once
#include "Playlist.hpp"
#include <functional>
#include <memory>
#include <atomic>
#include <mutex>
#include <thread>
#include <optional>

namespace rex::wallpaper {
class IWallpaperEngine;
class PlaylistManager;
}

namespace rex::wallpaper {

/// Playback lifecycle events surfaced to the host: "started", "item"
/// (index/path/type), "stopped", "finished" (end of playlist, no loop).
using PlaylistEventFn = std::function<void(const std::string& event,
                                           size_t index,
                                           const std::string& path,
                                           const std::string& type)>;

class PlaylistPlayer {
public:
    /// playlistManager is optional; without it the current position is not
    /// persisted (no resume across restarts).
    PlaylistPlayer(std::shared_ptr<IWallpaperEngine> engine,
                   std::shared_ptr<PlaylistManager> playlistManager = nullptr);
    ~PlaylistPlayer();

    /// Start playback. With resume=true and a valid playlist.lastIndex the
    /// player continues from that item instead of startIndex (or 0).
    bool Start(const Playlist& playlist, size_t startIndex = 0,
               bool resume = false);
    void Stop();
    void Next();
    void Previous();
    bool IsRunning() const { return m_running; }
    Playlist GetCurrentPlaylist() const;
    size_t GetCurrentIndex() const { return m_currentIndex; }

    void SetEventSink(PlaylistEventFn fn) { m_eventSink = std::move(fn); }

private:
    void PlayerLoop();
    void LoadAndPlay(size_t index);
    double GetItemDuration(const PlaylistItem& item);
    size_t NextIndex(size_t current, bool forward);
    void PersistCurrentPosition();

    std::shared_ptr<IWallpaperEngine> m_engine;
    std::shared_ptr<PlaylistManager> m_playlistManager;
    Playlist m_playlist;
    size_t m_currentIndex = 0;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};
    std::thread m_thread;
    mutable std::mutex m_mutex;
    PlaylistEventFn m_eventSink;
};

} // namespace rex::wallpaper
``

## src/wallpaper/playlist/Playlist.hpp

``cpp
#pragma once
#include <string>
#include <vector>
#include <rex/wallpaper/IWallpaperEngine.hpp>

namespace rex::wallpaper {

struct PlaylistItem {
    std::string type;       // "static", "video", "shader", ...
    std::string path;
    FillMode fillMode = FillMode::Fill;
    float volume = 1.0f;
    float speed = 1.0f;
    bool loop = true;       // per-item loop (if true, never advances)
};

struct Playlist {
    std::string id;
    std::string name;
    std::vector<PlaylistItem> items;
    bool shuffle = false;
    bool loopAll = true;          // when true, after last item go to first
    double intervalSeconds = 0.0; // 0 = use video duration; >0 overrides
    size_t lastIndex = 0;         // resume point: last played item
    std::string updatedAt;        // ISO-8601 UTC of the last position update
};

} // namespace rex::wallpaper
``

