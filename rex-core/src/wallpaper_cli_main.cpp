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

#include <psapi.h>

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

static void WriteFrame(const std::string& frame) {
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

static int RunRuntime(int maxFps, bool hardwareAcceleration) {
    // stdout is owned by the frame protocol â€” log to file only.
    rex::shared::Logger::Instance().SetConsoleOutput(false);
    rex::shared::Logger::Instance().Initialize(BuildLogPath(L"runtime.log"), rex::shared::LogLevel::Trace);

    REX_LOG_INFO("Runtime", "Wallpaper runtime started (maxFps={}, hwAccel={})",
                 maxFps, hardwareAcceleration);

    std::unique_ptr<IWallpaperEngine> engine;
    WallpaperConfig config;
    config.enableHardwareAcceleration = hardwareAcceleration;
    config.maxFps = maxFps;

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
            engine = CreateWallpaperEngine();
            auto result = engine->Initialize(config);
            if (result.IsError()) {
                engine.reset();
                REX_LOG_ERROR("Runtime", "Engine init failed: {}", result.Error().message.c_str());
                WriteResponse(id, false, result.Error().message);
                continue;
            }
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
    if (engine) engine->Shutdown();
    REX_LOG_INFO("Runtime", "Wallpaper runtime exiting (stdin closed)");
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    // Per-monitor DPI awareness MUST be set before any window creation or
    // display metric query. Without it the render window is sized in
    // virtualized (scaled) pixels inside a DWM-scaled desktop host, so it
    // covers only a fraction of the desktop (e.g. 2/3 on a 150% display).
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    std::set_terminate([]() {
        if (auto eptr = std::current_exception()) {
            try {
                std::rethrow_exception(eptr);
            } catch (const std::exception& ex) {
                fprintf(stderr, "TERMINATE: uncaught exception: %s\n", ex.what());
            } catch (...) {
                fprintf(stderr, "TERMINATE: unknown uncaught exception\n");
            }
        } else {
            fprintf(stderr, "TERMINATE: no exception (direct abort/terminate call)\n");
        }
        std::abort();
    });

    signal(SIGABRT, [](int) {
        fprintf(stderr, "ABORT signal caught! (exe base %p)\n", GetModuleHandleW(nullptr));
        void* frames[32];
        USHORT n = CaptureStackBackTrace(0, 32, frames, nullptr);
        for (USHORT i = 0; i < n; ++i) {
            HMODULE mod = nullptr;
            wchar_t modName[MAX_PATH] = {};
            if (GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(frames[i]), &mod)) {
                GetModuleFileNameW(mod, modName, MAX_PATH);
            }
            fprintf(stderr, "  %02u: %p (%ls)\n", i, frames[i], modName);
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
        } else if (arg == L"--help" || arg == L"-h") {
            std::cout <<
                "REX Wallpaper CLI\n"
                "Usage: rex-wallpaper-cli --file <path> [options]\n"
                "  --runtime               persistent mode: read commands from stdin,\n"
                "                          write responses/events to stdout (length-prefixed JSON)\n"
                "  --type <video|image|static> scene type (default: video)\n"
                "  --mode <fill|fit|stretch|center|tile>  fill mode\n"
                "  --fps <n>               max render fps (default: 60)\n"
                "  --no-loop               do not loop\n"
                "  --unmuted               enable audio\n"
                "  --volume <0..1>         playback volume\n"
                "  --speed <0.25..4>       playback speed\n"
                "  --no-hw                 disable hardware acceleration\n";
            return 0;
        }
    }

    if (runtimeMode) {
        return RunRuntime(maxFps, hardwareAcceleration);
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
