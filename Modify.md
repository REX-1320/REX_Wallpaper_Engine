# REX — Modify.md

Modified-file reference for the FFmpeg GPU-native video architecture
(D3D12VA primary → D3D11VA fallback → software fallback).

Each section lists the **file path** and the **full new content** of the file.
Sections at the end (`EngineFactory.cpp`, `tests/wallpaper/test_engine.cpp`)
require **no code change** — they are listed for completeness.

References to `NEW` files (e.g. `FfmpegVideoDecoder.cpp`) point at the
parallel "New Files" work items; the interfaces those files must expose are
documented inline with `// NEW:` comments.

---

## 1. `D:\REX_doc\rex-core\vcpkg.json`

Adds the `ffmpeg` dependency (v7+, required for D3D12VA hardware decode).

```json
{
  "name": "rex-core",
  "version": "0.1.0",
  "dependencies": [
    "fmt",
    "spdlog",
    "gtest",
    {
      "name": "ffmpeg",
      "default-features": false,
      "features": [
        "avcodec",
        "avformat",
        "avutil",
        "swscale"
      ]
    }
  ]
}
```

> Notes:
> - `default-features: false` drops OpenSSL/ALSA/etc. — the wallpaper runtime
>   only needs demuxing + decoding + swscale.
> - D3D11VA / DXVA2 / D3D12VA hwaccels are compiled in by the vcpkg ffmpeg
>   port by default (no extra features required).
> - After editing, run `vcpkg install` (or reconfigure CMake — the toolchain
>   installs missing packages automatically).

---

## 2. `D:\REX_doc\rex-core\CMakeLists.txt`

Dependency lookup, source list, and CLI link changes.

**Section `# ─── Dependencies ───` (after line 21 `find_package(spdlog ...)`):**

```cmake
find_package(spdlog CONFIG REQUIRED)

# FFmpeg (GPU-native video decode: D3D12VA -> D3D11VA -> software).
# vcpkg exports FFMPEG::avcodec / avformat / avutil / swscale targets.
find_package(FFMPEG CONFIG REQUIRED)
```

**Section `set(WALLPAPER_SOURCES ...)` (lines 46-55):**

```cmake
set(WALLPAPER_SOURCES
    src/wallpaper/engine/Engine.cpp
    src/wallpaper/engine/EngineFactory.cpp
    src/wallpaper/decoder/WICTextureLoader.cpp
    # NEW: FFmpeg decoder replaces MediaFoundationDecoder (removed).
    src/wallpaper/decoder/FfmpegVideoDecoder.cpp
    # NEW: GPU-frame playback replaces PlaybackController (removed).
    src/wallpaper/playback/VideoPlaybackController.cpp
    src/wallpaper/playback/FrameScheduler.cpp
    # NEW: persistent runtime (owns FFmpeg hw context + scheduler).
    src/wallpaper/runtime/VideoManager.cpp
    # NEW: GPU color pipeline (YUV->RGB / HDR / adjustments).
    src/wallpaper/render/VideoColorPipeline.cpp
    src/wallpaper/loader/ImageLoader.cpp
    src/wallpaper/scene/StaticImageScene.cpp
    src/wallpaper/runtime/RendererAdapters.cpp
)
```

> Removed from the list: `src/wallpaper/decoder/MediaFoundationDecoder.cpp`,
> `src/wallpaper/playback/PlaybackController.cpp`.

**Section `add_executable(rex-wallpaper-cli ...)` link list (lines 632-648):**

```cmake
target_link_libraries(rex-wallpaper-cli PRIVATE
    rex-shared
    fmt::fmt
    spdlog::spdlog
    d3d11
    d3d12
    dxgi
    dxguid
    d3dcompiler
    windowscodecs
    runtimeobject
    dwmapi
    winmm
    FFMPEG::avcodec
    FFMPEG::avformat
    FFMPEG::avutil
    FFMPEG::swscale
)
```

> Removed: `mfplat`, `mfreadwrite`, `mfuuid` — the Media Foundation
> dependency is gone.

---

## 3. `D:\REX_doc\rex-core\include\rex\wallpaper\IWallpaperEngine.hpp`

Telemetry gains the decoder path / codec / GPU-copy counters. The old
`ringSlotCount` / `ringBytes` fields are kept (they now report the bounded
GPU-frame queue depth and its VRAM footprint), so the CLI protocol stays
backward compatible.

```cpp
#pragma once

#include <shared/Result.hpp>
#include <shared/UUID.hpp>

#include <string>
#include <vector>
#include <memory>
#include <optional>

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

// ─── Hardware decode path ──────────────────────────────────────────────────
//
// Which backend the video pipeline is currently decoding with. Exposed in
// EngineTelemetry so the CLI/host can log:
//   Input codec: HEVC | HW path: D3D12VA | Decoder: hevc (d3d12va)

enum class VideoHardwarePath {
    None = 0,     // no video loaded
    Software,     // FFmpeg software decode (swscale)
    D3D11VA,      // FFmpeg D3D11VA hardware decode
    D3D12VA       // FFmpeg D3D12VA hardware decode (primary)
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
};

// ─── WallpaperConfig ───────────────────────────────────────────────────────

struct WallpaperConfig {
    bool  enableHardwareAcceleration = true;
    int   maxFps       = 60;
    int   targetMonitor = 0;
    bool  enableAudioReactive = false;
    float audioSensitivity    = 0.5f;
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

    // Bounded GPU-frame queue (was: CPU ring). ringSlotCount = queue depth,
    // ringBytes = sum of decoded surface bytes in the queue.
    uint64_t ringSlotCount   = 0;
    uint64_t ringBytes       = 0;

    // ─── Decoder identity (NEW) ─────────────────────────────────────────
    std::string videoCodec;    // "h264", "hevc", "av1", "vp9", ...
    std::string decoderName;   // e.g. "hevc (d3d12va)"
    VideoHardwarePath hwPath   = VideoHardwarePath::None;

    // ─── Zero-copy metric (NEW) ─────────────────────────────────────────
    // Targets for the hardware path: 0/frame.
    uint64_t gpuToCpuCopies = 0;   // GPU -> CPU readbacks per frame
    uint64_t cpuToGpuCopies = 0;   // CPU -> GPU uploads per frame

    // GPU (D3D12)
    uint64_t textureBytes    = 0;  // video texture committed bytes
    uint64_t uploadBytes     = 0;  // upload buffer bytes
    uint64_t textureRecreations = 0;  // texture re-creations (size changes)
    uint64_t uploads         = 0;  // total texture uploads performed
    uint64_t allocatorsPerFrame = 0; // command allocators created per upload

    // Reliability (NEW)
    uint64_t deviceLossCount = 0;  // D3D12 device-removed events handled
    uint64_t recoveryCount   = 0;  // successful recoveries

    // Scenes
    uint64_t scenesLoaded    = 0;
    uint64_t scenesReleased  = 0;

    // Process
    double   privateBytesMB  = 0.0;
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

    // ─── Settings ─────────────────────────────────────────────────────────
    virtual shared::Result<void> SetFillMode(FillMode mode) = 0;
    virtual shared::Result<void> SetBrightness(float value) = 0;
    virtual shared::Result<void> SetContrast(float value) = 0;
    virtual shared::Result<void> SetSaturation(float value) = 0;
    virtual shared::Result<void> SetVolume(float value) = 0;
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

    // ─── Telemetry ────────────────────────────────────────────────────────
    /// Snapshot of live allocation counters (decoder/queue/GPU/scenes).
    /// Cheap and non-blocking; safe to call from any thread.
    virtual void GetTelemetry(EngineTelemetry& out) const = 0;
};

// ─── Factory ───────────────────────────────────────────────────────────────

std::unique_ptr<IWallpaperEngine> CreateWallpaperEngine();

} // namespace rex::wallpaper
```

---

## 4. `D:\REX_doc\rex-core\src\wallpaper\runtime\RendererAdapters.cpp`

The video adapter now also accepts the GPU color-pipeline properties
(brightness / contrast / saturation), matching the new shader pipeline.

```cpp
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
    }
};

// Video: fill mode + GPU color pipeline (brightness/contrast/saturation)
// via the video shader, speed/loop via the playback controller.
// Volume/mute have no audio pipeline yet — reported unsupported.
class VideoAdapter : public BaseAdapter {
public:
    std::string_view GetType() const override { return "video"; }
    VideoAdapter() {
        m_supported.insert("fillMode");
        m_supported.insert("speed");
        m_supported.insert("loop");
        m_supported.insert("brightness");
        m_supported.insert("contrast");
        m_supported.insert("saturation");
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
```

---

## 5. `D:\REX_doc\rex-core\src\graphics\renderer\Device.hpp`

Adds a `CreateFence` factory (needed for decoder→renderer GPU sync) and an
adapter-LUID accessor (needed to create the FFmpeg D3D12VA device on the
same adapter).

```cpp
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
// signatures, pipeline states, committed resources, fences).
//
// Thread-safe: the underlying device is free-threaded.

class Device {
public:
    Device() = default;
    ~Device() = default;

    /// Create the D3D12 device with debug layer in Debug builds.
    /// @param minimumFeatureLevel Minimum feature level (default 11_0).
    shared::Result<void> Initialize(
        D3D_FEATURE_LEVEL minimumFeatureLevel = D3D_FEATURE_LEVEL_11_0);

    /// Returns the maximum supported feature level.
    D3D_FEATURE_LEVEL MaxFeatureLevel() const { return m_maxFeatureLevel; }

    /// Returns the DXGI adapter used by this device.
    Microsoft::WRL::ComPtr<IDXGIAdapter4> Adapter() const { return m_adapter; }

    /// LUID of the underlying adapter — used to pin the FFmpeg hardware
    /// decode device (D3D12VA / D3D11VA) to the same GPU.
    LUID AdapterLuid() const { return m_adapterLuid; }

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

    /// Create a fence for GPU synchronization (cross-queue/cross-device
    /// waiters are allowed on the same adapter).
    Microsoft::WRL::ComPtr<ID3D12Fence> CreateFence(
        UINT64 initialValue = 0,
        D3D12_FENCE_FLAGS flags = D3D12_FENCE_FLAG_NONE) const;

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
    LUID                                    m_adapterLuid{};
};

} // namespace rex::graphics
```

## 5b. `D:\REX_doc\rex-core\src\graphics\renderer\Device.cpp`

Only the two added members change. Full new content:

```cpp
#include "Device.hpp"

#include <shared/Logger.hpp>
#include <d3d12.h>

namespace rex::graphics {

shared::Result<void> Device::Initialize(D3D_FEATURE_LEVEL minimumFeatureLevel) {
    if (m_device) {
        return shared::Result<void>(shared::ErrorCode::AlreadyInitialized,
                                    "Device already initialized");
    }

#if defined(_DEBUG) || defined(REX_ENABLE_D3D12_DEBUG)
    {
        Microsoft::WRL::ComPtr<ID3D12Debug> debugInterface;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugInterface)))) {
            debugInterface->EnableDebugLayer();
        }
    }
#endif

    Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        return shared::Result<void>(shared::ErrorCode::UnknownError,
                                    "CreateDXGIFactory1 failed");
    }

    // Prefer a hardware adapter with the highest feature level.
    Microsoft::WRL::ComPtr<IDXGIAdapter4> adapter;
    D3D_FEATURE_LEVEL bestLevel = D3D_FEATURE_LEVEL_11_0;

    for (UINT i = 0;; ++i) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> candidate;
        if (factory->EnumAdapters1(i, &candidate) == DXGI_ERROR_NOT_FOUND) {
            break;
        }

        DXGI_ADAPTER_DESC1 desc{};
        candidate->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            continue;
        }

        // Query the max feature level this adapter supports.
        D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1,
            D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0
        };
        for (auto level : levels) {
            Microsoft::WRL::ComPtr<ID3D12Device> probe;
            if (SUCCEEDED(D3D12CreateDevice(candidate.Get(), level,
                                            IID_PPV_ARGS(&probe)))) {
                if (level >= bestLevel) {
                    bestLevel = level;
                    candidate.As(&adapter);
                }
                break;
            }
        }
        if (adapter && bestLevel >= D3D_FEATURE_LEVEL_12_0) {
            break; // good enough — first capable hardware adapter
        }
    }

    if (!adapter) {
        return shared::Result<void>(shared::ErrorCode::UnknownError,
                                    "No suitable D3D12 hardware adapter found");
    }

    hr = D3D12CreateDevice(adapter.Get(), bestLevel, IID_PPV_ARGS(&m_device));
    if (FAILED(hr)) {
        return shared::Result<void>(shared::ErrorCode::UnknownError,
                                    "D3D12CreateDevice failed");
    }

    m_adapter = adapter;
    m_maxFeatureLevel = bestLevel;
    DXGI_ADAPTER_DESC1 adapterDesc{};
    adapter->GetDesc1(&adapterDesc);
    m_adapterLuid = adapterDesc.Luid;

    REX_LOG_INFO("Graphics", "D3D12 device created (feature level {})",
                 static_cast<int>(bestLevel));
    return shared::Result<void>();
}

Microsoft::WRL::ComPtr<ID3D12CommandAllocator> Device::CreateCommandAllocator(
    D3D12_COMMAND_LIST_TYPE type) const {
    if (!m_device) return nullptr;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    if (FAILED(m_device->CreateCommandAllocator(type, IID_PPV_ARGS(&allocator)))) {
        return nullptr;
    }
    return allocator;
}

Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> Device::CreateCommandList(
    ID3D12CommandAllocator* allocator, ID3D12PipelineState* initialPso) const {
    if (!m_device || !allocator) return nullptr;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           allocator, initialPso,
                                           IID_PPV_ARGS(&list)))) {
        return nullptr;
    }
    return list;
}

Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> Device::CreateDescriptorHeap(
    D3D12_DESCRIPTOR_HEAP_TYPE type, UINT numDescriptors,
    D3D12_DESCRIPTOR_HEAP_FLAGS flags) const {
    if (!m_device) return nullptr;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.NumDescriptors = numDescriptors;
    desc.Type = type;
    desc.Flags = flags;
    if (FAILED(m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)))) {
        return nullptr;
    }
    return heap;
}

Microsoft::WRL::ComPtr<ID3D12RootSignature> Device::CreateRootSignature(
    const void* serializedBlob, SIZE_T blobSize) const {
    if (!m_device) return nullptr;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> signature;
    if (FAILED(m_device->CreateRootSignature(0, serializedBlob, blobSize,
                                             IID_PPV_ARGS(&signature)))) {
        return nullptr;
    }
    return signature;
}

Microsoft::WRL::ComPtr<ID3D12PipelineState> Device::CreateGraphicsPipelineState(
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc) const {
    if (!m_device) return nullptr;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
    if (FAILED(m_device->CreateGraphicsPipelineState(&desc,
                                                     IID_PPV_ARGS(&pso)))) {
        return nullptr;
    }
    return pso;
}

Microsoft::WRL::ComPtr<ID3D12Resource> Device::CreateCommittedResource(
    const D3D12_HEAP_PROPERTIES& heapProps, D3D12_HEAP_FLAGS flags,
    const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES initial,
    const D3D12_CLEAR_VALUE* clearValue) const {
    if (!m_device) return nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    if (FAILED(m_device->CreateCommittedResource(&heapProps, flags, &desc,
                                                 initial, clearValue,
                                                 IID_PPV_ARGS(&resource)))) {
        return nullptr;
    }
    return resource;
}

Microsoft::WRL::ComPtr<ID3D12Fence> Device::CreateFence(
    UINT64 initialValue, D3D12_FENCE_FLAGS flags) const {
    if (!m_device) return nullptr;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    if (FAILED(m_device->CreateFence(initialValue, flags,
                                     IID_PPV_ARGS(&fence)))) {
        return nullptr;
    }
    return fence;
}

Microsoft::WRL::ComPtr<IDXGIFactory6> Device::CreateFactory() const {
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return nullptr;
    }
    return factory;
}

} // namespace rex::graphics
```

---

## 6. `D:\REX_doc\rex-core\src\graphics\renderer\CommandQueue.hpp`

Adds cross-queue fence signaling/waiting — the decoder's GPU work (FFmpeg
D3D12VA queue) is ordered against the renderer's queue via a shared fence.

```cpp
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

    /// Signal the fence, returning the new fence value.
    UINT64 Signal();

    // ─── External fence sync (NEW) ───────────────────────────────────────
    // Used to order the decoder's hardware-decode queue against this
    // (render) queue. Fences are per-adapter, so a fence created on the
    // decoder device can be waited on here (same GPU).
    /// Make this queue signal an external fence with `value`.
    void SignalFence(ID3D12Fence* fence, UINT64 value);

    /// Make this queue wait until an external fence reaches `value`.
    void WaitForFence(ID3D12Fence* fence, UINT64 value);

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
```

## 6b. `D:\REX_doc\rex-core\src\graphics\renderer\CommandQueue.cpp`

Full new content (existing methods unchanged, two added):

```cpp
#include "CommandQueue.hpp"

#include <shared/Logger.hpp>

namespace rex::graphics {

CommandQueue::~CommandQueue() { Shutdown(); }

shared::Result<void> CommandQueue::Initialize(ID3D12Device* device) {
    if (m_queue) {
        return shared::Result<void>(shared::ErrorCode::AlreadyInitialized,
                                    "Command queue already initialized");
    }
    if (!device) {
        return shared::Result<void>(shared::ErrorCode::InvalidArgument,
                                    "Command queue requires a device");
    }

    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT;
    desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    desc.Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE;
    desc.NodeMask = 0;

    HRESULT hr = device->CreateCommandQueue(&desc, IID_PPV_ARGS(&m_queue));
    if (FAILED(hr)) {
        return shared::Result<void>(shared::ErrorCode::UnknownError,
                                    "CreateCommandQueue failed");
    }

    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
    if (FAILED(hr)) {
        m_queue.Reset();
        return shared::Result<void>(shared::ErrorCode::UnknownError,
                                    "CreateFence failed");
    }

    m_fenceValue = 0;
    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent) {
        m_queue.Reset();
        m_fence.Reset();
        return shared::Result<void>(shared::ErrorCode::UnknownError,
                                    "CreateEvent failed");
    }

    return shared::Result<void>();
}

void CommandQueue::Shutdown() {
    if (m_queue) {
        // Flush outstanding work before tearing down.
        m_fenceValue++;
        m_queue->Signal(m_fence.Get(), m_fenceValue);
        if (m_fence->GetCompletedValue() < m_fenceValue) {
            m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
            WaitForSingleObject(m_fenceEvent, INFINITE);
        }
    }
    if (m_fenceEvent) {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }
    m_fence.Reset();
    m_queue.Reset();
}

void CommandQueue::Execute(ID3D12CommandList* const* lists, UINT count) {
    if (!m_queue || !lists || count == 0) return;
    m_queue->ExecuteCommandLists(count, lists);
}

void CommandQueue::Execute(ID3D12CommandList* list) {
    if (!m_queue || !list) return;
    ID3D12CommandList* lists[] = { list };
    m_queue->ExecuteCommandLists(1, lists);
}

void CommandQueue::WaitForGPU() {
    if (!m_queue || !m_fence || !m_fenceEvent) return;
    m_fenceValue++;
    m_queue->Signal(m_fence.Get(), m_fenceValue);
    if (m_fence->GetCompletedValue() < m_fenceValue) {
        m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

UINT64 CommandQueue::Signal() {
    if (!m_queue || !m_fence) return 0;
    m_fenceValue++;
    m_queue->Signal(m_fence.Get(), m_fenceValue);
    return m_fenceValue;
}

void CommandQueue::SignalFence(ID3D12Fence* fence, UINT64 value) {
    if (!m_queue || !fence) return;
    m_queue->Signal(fence, value);
}

void CommandQueue::WaitForFence(ID3D12Fence* fence, UINT64 value) {
    if (!m_queue || !fence) return;
    m_queue->Wait(fence, value);
}

} // namespace rex::graphics
```

---

## 7. `D:\REX_doc\rex-core\src\wallpaper_cli_main.cpp`

Persistent-session protocol with decoder diagnostics. The runtime already
stays alive across `LOAD` commands (only stream resources are swapped
inside the engine); this change adds the hardware-path report to `LOAD`
responses and `GET_STATUS` telemetry.

Full new content:

```cpp
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

// Process private (committed) memory in MB — for the frontend telemetry.
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

// ─── Hardware-path name (for diagnostics JSON) ──────────────────────────────

static const char* HwPathName(VideoHardwarePath path) {
    switch (path) {
        case VideoHardwarePath::D3D12VA: return "d3d12va";
        case VideoHardwarePath::D3D11VA: return "d3d11va";
        case VideoHardwarePath::Software: return "software";
        case VideoHardwarePath::None:
        default: return "none";
    }
}

// ─── Runtime frame protocol ─────────────────────────────────────────────────
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

// Decoder diagnostics for the LOAD response / GET_STATUS telemetry:
//   Input codec: HEVC | HW path: D3D12VA | Decoder: hevc (d3d12va)
static json::Json DecoderDiagnostics(const EngineTelemetry& t) {
    json::Json::Object d;
    d.emplace("codec", json::Json(t.videoCodec));
    d.emplace("hwPath", json::Json(HwPathName(t.hwPath)));
    d.emplace("decoder", json::Json(t.decoderName));
    d.emplace("gpuToCpuCopies", json::Json(static_cast<double>(t.gpuToCpuCopies)));
    d.emplace("cpuToGpuCopies", json::Json(static_cast<double>(t.cpuToGpuCopies)));
    return json::Json(std::move(d));
}

static int RunRuntime(int maxFps, bool hardwareAcceleration) {
    // stdout is owned by the frame protocol — log to file only.
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

            // Images go through the OS wallpaper API — no engine needed.
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
                WriteResponse(id, false, "runtime not initialized — send INIT first");
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

            // ── Decoder diagnostics (NEW) ────────────────────────────────
            // Reported in the LOAD response so the host can display:
            //   "HEVC | D3D12VA | hevc (d3d12va)"
            json::Json::Object data;
            data.emplace("path", json::Json(path));
            data.emplace("type", json::Json(type));
            if (type == "video") {
                EngineTelemetry tel;
                engine->GetTelemetry(tel);
                data.emplace("decoder", DecoderDiagnostics(tel));
            }
            WriteResponse(id, true, "", json::Json(std::move(data)));
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

            // ── Telemetry ────────────────────────────────────────────────
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
            // Decoder identity + zero-copy counters (NEW)
            tel.emplace("codec", json::Json(t.videoCodec));
            tel.emplace("hwPath", json::Json(HwPathName(t.hwPath)));
            tel.emplace("decoder", json::Json(t.decoderName));
            tel.emplace("gpuToCpuCopies", json::Json(static_cast<double>(t.gpuToCpuCopies)));
            tel.emplace("cpuToGpuCopies", json::Json(static_cast<double>(t.cpuToGpuCopies)));
            tel.emplace("textureBytes", json::Json(static_cast<double>(t.textureBytes)));
            tel.emplace("uploadBytes", json::Json(static_cast<double>(t.uploadBytes)));
            tel.emplace("textureRecreations", json::Json(static_cast<double>(t.textureRecreations)));
            tel.emplace("uploads", json::Json(static_cast<double>(t.uploads)));
            tel.emplace("allocatorsPerFrame", json::Json(static_cast<double>(t.allocatorsPerFrame)));
            tel.emplace("deviceLossCount", json::Json(static_cast<double>(t.deviceLossCount)));
            tel.emplace("recoveryCount", json::Json(static_cast<double>(t.recoveryCount)));
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

    // stdin closed (host quit without SHUTDOWN) — clean up.
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

    // Non-runtime mode: print the decoder diagnostics once at load.
    {
        EngineTelemetry tel;
        engine->GetTelemetry(tel);
        printf("[REX] Input codec: %s | HW path: %s | Decoder: %s\n",
               tel.videoCodec.c_str(), HwPathName(tel.hwPath),
               tel.decoderName.c_str());
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
```

---

## 8. `D:\REX_doc\rex-core\src\wallpaper\engine\Engine.hpp`

`m_playback` (PlaybackController) is replaced by the persistent
`VideoManager` (GPU-frame pipeline). Device-loss handling becomes
explicit.

```cpp
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
// NEW: persistent video runtime (FFmpeg decoder + scheduler + GPU frames).
#include "wallpaper/runtime/VideoManager.hpp"
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
    shared::Result<void> SetLoop(bool loop) override;
    shared::Result<void> SetSpeed(float speed) override;

    shared::Result<void> SetProperty(const std::string& property, const std::string& value) override;
    std::vector<std::string> GetSupportedProperties() const override;
    std::string GetRendererType() const override;

    shared::Result<void> EnableAudioReactive(bool enable) override;
    shared::Result<void> SetAudioSensitivity(float sensitivity) override;

    void DumpStats() const override;
    void GetTelemetry(EngineTelemetry& out) const override;
    shared::UUID GetSessionId() const override { return m_sessionId; }

private:
    void Update(double deltaTime);
    void SetState(EngineState state);
    void RenderLoop();
    shared::Result<void> ReinitializeRenderer();
    /// Device-removed recovery: preserve playback position, recreate the
    /// GPU resources, then resume from the saved timestamp.
    shared::Result<void> HandleDeviceLoss();
    void CheckDisplayChanges();

    // Atomic so the render thread can safely read it while the command
    // thread swaps scenes (UnloadScene/LoadScene) under the mutex.
    std::atomic<EngineState> m_state{EngineState::Uninitialized};
    std::atomic<bool> m_playing{false};
    std::atomic<bool> m_loop{true};
    std::atomic<float> m_speed{1.0f};

    SceneInfo m_currentScene;
    MonitorInfo m_currentMonitor;
    WallpaperConfig m_config;

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
    // NEW: persistent video runtime — survives scene swaps; only the
    // stream-specific decoder state is replaced per LOAD.
    std::unique_ptr<VideoManager> m_videoManager;
    std::unique_ptr<IRendererAdapter> m_adapter;
    std::thread m_renderThread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_recoveryNeeded{false};
};

} // namespace rex::wallpaper
```

---

## 9. `D:\REX_doc\rex-core\src\wallpaper\engine\Engine.cpp`

Video scenes go through the persistent `VideoManager`; the render loop
draws GPU frames zero-copy; device loss preserves playback position.

```cpp
#include "Engine.hpp"
#include "platform/windows/DesktopHostFactory.hpp"
#include <iostream>
#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <psapi.h>

namespace rex::wallpaper {

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

    // Acquire desktop host (tries WorkerW → Progman → error)
    // Use a temporary unique_ptr so the host is released on any early return.
    auto host = rex::platform::windows::DesktopHostFactory::CreateBestAvailable();
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

    // NEW: persistent video runtime — owns the FFmpeg hardware context
    // (reused across scene switches) and the frame scheduler.
    m_videoManager = std::make_unique<VideoManager>();
    auto vmResult = m_videoManager->Initialize(m_config);
    if (vmResult.IsError()) {
        REX_LOG_WARN("Engine", "VideoManager init failed (video scenes degraded): {}",
                     vmResult.Error().message);
    }

    // Success — transfer ownership to member
    m_desktopHost = std::move(host);

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
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_state == EngineState::Uninitialized) {
        // Defensive: clean up anything that may have been allocated during
        // a failed Initialize (e.g. renderer failure after host acquisition).
        if (m_desktopHost) {
            m_desktopHost->Release();
            m_desktopHost.reset();
        }
        m_renderer.reset();
        m_videoManager.reset();
        return shared::Result<void>();
    }

    REX_LOG_INFO("Engine", "Shutting down...");

    SetState(EngineState::ShuttingDown);

    m_desktopHost->StopMonitor();
    m_running = false;
    if (m_renderThread.joinable()) {
        m_renderThread.join();
    }

    m_videoManager.reset();
    m_renderer.reset();
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

    // Destroy old renderer
    if (m_videoManager) {
        m_videoManager->CloseVideo();
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

    // LoadScene only accepts Initialized/Stopped states; the engine is in
    // Error during recovery, so move to Initialized before reloading.
    SetState(EngineState::Initialized);

    // Reload scene
    if (!m_currentScene.sourcePath.empty()) {
        auto loadResult = LoadScene(m_currentScene);
        if (loadResult.IsError()) {
            REX_LOG_WARN("Engine", "Failed to reload scene after recovery: {}",
                         loadResult.Error().message);
        }
    }

    REX_LOG_INFO("Engine", "Renderer reinitialized successfully");
    return shared::Result<void>();
}

// NEW: device-removed recovery — preserve playback state, recreate GPU
// resources, reopen the same stream and resume from the saved position.
shared::Result<void> Engine::HandleDeviceLoss() {
    REX_LOG_INFO("Engine", "Handling device loss...");

    double resumePosition = 0.0;
    SceneInfo scene = m_currentScene;
    bool wasPlaying = m_playing.load();

    if (m_videoManager && m_videoManager->IsOpen()) {
        resumePosition = m_videoManager->GetPosition();
        m_videoManager->CloseVideo();
    }

    auto result = ReinitializeRenderer();
    if (result.IsError()) {
        REX_LOG_ERROR("Engine", "Device-loss recovery failed: {}",
                      result.Error().message);
        return result;
    }

    if (!scene.sourcePath.empty() && scene.type == "video" && m_videoManager) {
        auto reopen = m_videoManager->OpenVideo(scene.sourcePath);
        if (reopen.IsError()) {
            REX_LOG_WARN("Engine", "Reopen after device loss failed: {}",
                         reopen.Error().message);
            return result;
        }
        m_videoManager->SetLoop(scene.loop);
        m_videoManager->SetSpeed(m_speed.load());
        if (resumePosition > 0.0) {
            m_videoManager->Seek(resumePosition);
        }
    }

    if (wasPlaying && !scene.sourcePath.empty()) {
        Play();
    }

    REX_LOG_INFO("Engine", "Device-loss recovery complete (resumed at {:.2f}s)",
                 resumePosition);
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
    } else if (scene.type == "video" && !scene.sourcePath.empty()) {
        if (!m_videoManager) {
            REX_LOG_WARN("Engine", "VideoManager unavailable — video scene ignored");
        } else {
            auto videoResult = m_videoManager->OpenVideo(scene.sourcePath);
            if (videoResult.IsError()) {
                REX_LOG_WARN("Engine", "Failed to open video '{}': {}",
                             scene.sourcePath.c_str(), videoResult.Error().message);
            } else {
                // Apply scene-level playback settings immediately.
                m_videoManager->SetLoop(scene.loop);
                m_videoManager->SetSpeed(std::clamp(scene.speed, 0.25f, 4.0f));
                // Push the GPU color pipeline adjustments.
                m_renderer->SetVideoAdjustments(scene.brightness, scene.contrast,
                                                scene.saturation);
                // Decoder diagnostics (codec / hw path / decoder name).
                auto diag = m_videoManager->GetDecoderInfo();
                REX_LOG_INFO("Engine", "Input codec: {} | HW path: {} | Decoder: {}",
                             diag.codec.c_str(), diag.hwPath.c_str(), diag.decoderName.c_str());
            }
        }
    }

    SetState(EngineState::Stopped);
    return shared::Result<void>();
}

shared::Result<void> Engine::UnloadScene() {
    std::lock_guard<std::mutex> lock(m_mutex);

    REX_LOG_INFO("Engine", "Unloading scene");

    m_scenesReleased.fetch_add(1, std::memory_order_relaxed);

    if (m_videoManager) {
        m_videoManager->CloseVideo();
    }

    if (m_renderer) {
        m_renderer->ClearTexture();
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
    if (m_playing && m_videoManager && m_videoManager->IsOpen()) {
        return m_videoManager->GetPosition();
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
    REX_LOG_DEBUG("Engine", "Fill mode set to {}", static_cast<int>(mode));
    return shared::Result<void>();
}

// GPU color pipeline: brightness/contrast/saturation now live in the video
// shader (no CPU passes).
shared::Result<void> Engine::SetBrightness(float value) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_currentScene.brightness = std::clamp(value, 0.0f, 2.0f);
    if (m_renderer) {
        m_renderer->SetVideoAdjustments(m_currentScene.brightness,
                                        m_currentScene.contrast,
                                        m_currentScene.saturation);
    }
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
    REX_LOG_DEBUG("Engine", "Saturation set to {:.2f}", m_currentScene.saturation);
    return shared::Result<void>();
}

shared::Result<void> Engine::SetVolume(float value) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_currentScene.volume = std::clamp(value, 0.0f, 1.0f);
    REX_LOG_DEBUG("Engine", "Volume set to {:.2f}", m_currentScene.volume);
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
    if (property == "speed") {
        return SetSpeed(static_cast<float>(std::atof(value.c_str())));
    }
    if (property == "loop") {
        return SetLoop(value == "true" || value == "1");
    }
    // NEW: GPU color pipeline properties.
    if (property == "brightness") {
        return SetBrightness(static_cast<float>(std::atof(value.c_str())));
    }
    if (property == "contrast") {
        return SetContrast(static_cast<float>(std::atof(value.c_str())));
    }
    if (property == "saturation") {
        return SetSaturation(static_cast<float>(std::atof(value.c_str())));
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

void Engine::GetTelemetry(EngineTelemetry& out) const {
    out = EngineTelemetry{};

    if (m_videoManager && m_videoManager->IsOpen()) {
        auto pt = m_videoManager->GetTelemetry();
        out.framesDecoded  = pt.framesDecoded;
        out.framesConsumed = pt.framesConsumed;
        out.framesDropped  = pt.framesDropped;
        out.decoderFps     = pt.decoderFps;
        out.renderFps      = pt.renderFps;
        out.ringSlotCount  = pt.queueDepth;
        out.ringBytes      = pt.queueBytes;
        // Decoder identity / zero-copy counters (NEW).
        out.videoCodec     = pt.videoCodec;
        out.decoderName    = pt.decoderName;
        out.hwPath         = pt.hwPath;
        out.gpuToCpuCopies = pt.gpuToCpuCopies;
        out.cpuToGpuCopies = pt.cpuToGpuCopies;
    }
    if (m_renderer) {
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

    out.deviceLossCount = m_deviceResetCount;
    out.recoveryCount   = m_recoveryCount;

    out.scenesLoaded   = m_scenesLoaded.load(std::memory_order_relaxed);
    out.scenesReleased = m_scenesReleased.load(std::memory_order_relaxed);

    PROCESS_MEMORY_COUNTERS_EX pmc = {};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(),
            reinterpret_cast<PPROCESS_MEMORY_COUNTERS>(&pmc), sizeof(pmc))) {
        out.privateBytesMB = static_cast<double>(pmc.PrivateUsage)
                           / (1024.0 * 1024.0);
    }
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

    if (m_videoManager && m_videoManager->IsOpen()) {
        auto tel = m_videoManager->GetTelemetry();
        REX_LOG_INFO("Engine", "  ── Playback ──────");
        REX_LOG_INFO("Engine", "  Codec:           {}", tel.videoCodec.c_str());
        REX_LOG_INFO("Engine", "  HW path:         {}", tel.hwPath.c_str());
        REX_LOG_INFO("Engine", "  Decoder:         {}", tel.decoderName.c_str());
        REX_LOG_INFO("Engine", "  Frames decoded:  {}", tel.framesDecoded);
        REX_LOG_INFO("Engine", "  Frames consumed: {}", tel.framesConsumed);
        REX_LOG_INFO("Engine", "  Frames dropped:  {}", tel.framesDropped);
        REX_LOG_INFO("Engine", "  Decoder FPS:     {:.1f}", tel.decoderFps);
        REX_LOG_INFO("Engine", "  Render FPS:      {:.1f}", tel.renderFps);
        REX_LOG_INFO("Engine", "  Queue depth:     {} ({} bytes)", tel.queueDepth, tel.queueBytes);
        REX_LOG_INFO("Engine", "  GPU→CPU copies:  {}", tel.gpuToCpuCopies);
        REX_LOG_INFO("Engine", "  CPU→GPU copies:  {}", tel.cpuToGpuCopies);
    }
}

void Engine::Update(double deltaTime) {
    if (m_state != EngineState::Running) return;

    m_frameCount++;
    m_frameTime = deltaTime;

    if (m_playing) {
        double elapsed = GetPosition();
        m_currentPosition = elapsed;

        if (m_frameCount % 60 == 0) {
            REX_LOG_DEBUG("Engine", "Frame {}: {:.2f} s, {:.2f} ms",
                          m_frameCount, elapsed, deltaTime);
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
            bool wasPlaying = m_playing.load();
            SetState(EngineState::Error);
            m_recoveryCount++;

            auto recoveryResult = ReinitializeRenderer();
            if (recoveryResult.IsSuccess()) {
                if (wasPlaying) Play();
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
            m_renderer->BeginFrame();

            // ── Video frame update (zero-copy GPU path) ────────────────
            // Tick() returns the frame the scheduler picked for this
            // present time; the renderer draws the decoder's GPU surface
            // directly (no CPU upload on the hardware path).
            if (m_state == EngineState::Running && m_videoManager
                && m_currentScene.type == "video") {
                const VideoFrame* frame = m_videoManager->Tick();
                if (frame && frame->valid) {
                    m_renderer->DrawVideoFrame(*frame);
                }
            }

            // ── Clear color (static dark until texture is loaded) ──────
            if (m_state == EngineState::Running
                && !m_renderer->HasTexture()
                && !m_renderer->HasVideoFrame()) {
                m_renderer->SetClearColor(0.02f, 0.02f, 0.02f);
            }

            m_renderer->RenderFrame();
            m_renderer->EndFrame();

            auto frameEnd = high_resolution_clock::now();
            double deltaMs = duration<double, std::milli>(
                frameEnd - frameStart).count();
            Update(deltaMs / 1000.0);
        } else {
            // Renderer not ready — wait before retrying
            std::this_thread::sleep_for(milliseconds(16));
        }
    }

    REX_LOG_INFO("Engine", "Render loop ended");
}

} // namespace rex::wallpaper
```

---

## 10. `D:\REX_doc\rex-core\src\graphics\renderer\D3D12Renderer.hpp`

The renderer gains the GPU-native video path: `DrawVideoFrame()` consumes the
decoder's shared GPU surface (zero-copy hardware path, `D3D12VA`/`D3D11VA`)
and falls back to the existing CPU-upload path when the software decoder ran.

`VideoFrame` is defined here (canonical). The NEW `VideoManager.hpp` includes
this header (`#include "graphics/renderer/D3D12Renderer.hpp"`) so its
`Tick()` can produce frames.

```cpp
#pragma once

#include "Device.hpp"
#include "SwapChain.hpp"
#include "CommandQueue.hpp"
#include "DescriptorHeapManager.hpp"
#include "wallpaper/decoder/WICTextureLoader.hpp"

#include <shared/Result.hpp>
#include <rex/wallpaper/IWallpaperEngine.hpp>

#include <mutex>
#include <atomic>
#include <string_view>

namespace rex::wallpaper {
using namespace rex::graphics;

// ─── VideoFrame ──────────────────────────────────────────────────────────
//
// A decoded video frame ready for presentation.
//
// Hardware path (primary): `sharedHandle` is the NT handle of the decoder's
// GPU surface. The surface is RGBA8 (VideoColorPipeline's YUV->RGB pass
// produces it, created with D3D12_HEAP_FLAG_SHARED and an NT handle via
// CreateSharedHandle, or D3D11_RESOURCE_MISC_SHARED_NTHANDLE for the
// D3D11VA fallback). The renderer opens it with its own device via
// ID3D12Device::OpenSharedHandle and draws it directly — no CPU upload.
//
// Software fallback: `pixels` / `width` / `height` / `rowPitch` carry CPU
// RGBA8 data consumed by UpdateTexturePixels().
//
// Contract notes for VideoManager (NEW file):
//  - `valid` must be false for a skipped/empty frame slot.
//  - Handles are recycled by the decoder; handing back the same handle for
//    a reused surface lets the renderer reuse the SRV (one open per handle).

struct VideoFrame {
    bool valid = false;
    uint64_t frameIndex = 0;   // monotonically increasing frame number
    double pts = 0.0;          // presentation timestamp (seconds)

    // Hardware path (zero-copy GPU surface).
    HANDLE sharedHandle = nullptr;

    // Software fallback (CPU pixels).
    const uint8_t* pixels = nullptr;
    int width  = 0;
    int height = 0;
    int rowPitch = 0;
};

// ─── D3D12Renderer ─────────────────────────────────────────────────────────
//
// Wallpaper-focused D3D12 renderer. Renders a fullscreen textured quad
// with configurable fill modes. For video playback, supports efficient
// per-frame texture updates via UpdateTexturePixels() and the GPU-native
// DrawVideoFrame() path (zero-copy on hardware decode).
//
// Uses the shared rex::graphics primitives (Device, SwapChain, CommandQueue).

class D3D12Renderer {
public:
    D3D12Renderer() = default;
    ~D3D12Renderer();

    shared::Result<void> Initialize(HWND targetWindow, int width, int height);
    void Shutdown();
    void Resize(int width, int height);

    // ─── Texture Loading ────────────────────────────────────────────────
    shared::Result<void> LoadTexture(std::string_view path);
    shared::Result<void> UpdateTexturePixels(
        const uint8_t* pixels, int width, int height, int rowPitch);
    void ClearTexture();
    bool HasTexture() const { return m_hasTexture; }

    // ─── Video Frame (GPU-native path) ────────────────────────────────
    // Called between BeginFrame()/EndFrame() with the frame returned by
    // VideoManager::Tick(). Hardware path: draws the decoder's shared GPU
    // surface directly (zero-copy). Software path: falls back to
    // UpdateTexturePixels().
    shared::Result<void> DrawVideoFrame(const VideoFrame& frame);
    bool HasVideoFrame() const { return m_hasVideoFrame; }

    // ─── Fill Mode ──────────────────────────────────────────────────────
    void SetFillMode(FillMode mode) { m_fillMode = mode; }
    FillMode GetFillMode() const { return m_fillMode; }

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
    bool CreatePipelineState();
    bool CreateVertexBuffer();

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
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_pipelineState;
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

    // ─── Video frame state (decoder GPU surface) ────────────────────────
    // SRV slot 1 of m_srvHeap (slot 0 is the static texture). The decoder
    // recycles surfaces; keeping the last opened handle avoids re-opening
    // and re-creating an SRV every frame.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_videoTexture;
    D3D12_GPU_DESCRIPTOR_HANDLE            m_videoSRVGpuHandle{};
    HANDLE m_videoSharedHandle = nullptr;
    bool   m_hasVideoFrame = false;
    int    m_videoWidth  = 0;
    int    m_videoHeight = 0;

    // ─── Fill mode ─────────────────────────────────────────────────────
    FillMode m_fillMode = FillMode::Fill;

    // ─── Dimensions & state ───────────────────────────────────────────
    int   m_width = 0;
    int   m_height = 0;
    float m_clearColor[4] = {0.1f, 0.1f, 0.2f, 1.0f};
    bool  m_initialized  = false;
    bool  m_deviceRemoved = false;

    // ─── Thread safety ────────────────────────────────────────────────
    mutable std::mutex m_renderMutex;

    // ─── Telemetry ─────────────────────────────────────────────────────
    std::atomic<uint64_t> m_textureBytes{0};
    std::atomic<uint64_t> m_uploadBytes{0};
    std::atomic<uint64_t> m_textureRecreations{0};
    std::atomic<uint64_t> m_uploads{0};
    std::atomic<uint64_t> m_allocatorsPerUpload{2}; // copy list + render frame

    // ─── Vertex layout ────────────────────────────────────────────────
    struct Vertex { float x, y, u, v; };
};

} // namespace rex::wallpaper
```

---

## 10b. `D:\REX_doc\rex-core\src\graphics\renderer\D3D12Renderer.cpp`

Adds `DrawVideoFrame()` (hardware shared-surface path + software fallback)
and makes `RenderFrame()`/`ComputeFillViewport()` prefer the active video
surface. `ClearTexture()` and `Shutdown()` also release video state.

```cpp
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
VSOutput main(VSInput input) {
    VSOutput output;
    output.pos = float4(input.pos, 0.0f, 1.0f);
    output.uv = input.uv;
    return output;
}
)";

static const char* g_pixelShaderSource = R"(
Texture2D tex : register(t0);
SamplerState sam : register(s0);
struct VSOutput {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD;
};
float4 main(VSOutput input) : SV_TARGET {
    return tex.Sample(sam, input.uv);
}
)";

// ─── Constructor / Destructor ──────────────────────────────────────────────

D3D12Renderer::~D3D12Renderer() { Shutdown(); }

// ─── Initialize ───────────────────────────────────────────────────────────

shared::Result<void> D3D12Renderer::Initialize(HWND targetWindow, int width, int height) {
    if (m_initialized) {
        return shared::Result<void>(shared::ErrorCode::AlreadyInitialized,
                                    "Renderer already initialized");
    }

    REX_LOG_INFO("D3D12", "Initializing D3D12 renderer ({}x{})", width, height);

    m_width  = width;
    m_height = height;

    // ── Device ─────────────────────────────────────────────────────────
    auto devResult = m_device.Initialize();
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
    if (!CreatePipelineState()) {
        return shared::Result<void>(shared::ErrorCode::UnknownError,
                                    "Failed to create pipeline state");
    }

    // ── Initialize command list ───────────────────────────────────────
    m_commandList = m_device.CreateCommandList(
        m_commandAllocator.Get(), m_pipelineState.Get());
    if (!m_commandList) {
        return shared::Result<void>(shared::ErrorCode::UnknownError,
                                    "Failed to create command list");
    }
    m_commandList->Close();

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
    m_videoTexture.Reset();
    m_videoSharedHandle = nullptr;
    m_hasVideoFrame = false;
    m_vertexBuffer.Reset();
    m_pipelineState.Reset();
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
    // A detached video surface is also dropped here (scene switch).
    m_videoTexture.Reset();
    m_videoSharedHandle = nullptr;
    m_hasVideoFrame = false;
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

    // Create temp copy command list
    auto copyAllocator = m_device.CreateCommandAllocator();
    if (!copyAllocator) {
        if (IsDeviceRemovedHr(m_device.GetDeviceRemovedReason())) m_deviceRemoved = true;
        return shared::Result<void>(shared::ErrorCode::UnknownError,
                                    "CreateCommandAllocator failed");
    }

    auto copyList = m_device.CreateCommandList(copyAllocator.Get());
    if (!copyList) {
        if (IsDeviceRemovedHr(m_device.GetDeviceRemovedReason())) m_deviceRemoved = true;
        return shared::Result<void>(shared::ErrorCode::UnknownError,
                                    "CreateCommandList failed");
    }

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

// ─── Draw Video Frame (GPU-native, zero-copy hardware path) ──────────────

shared::Result<void> D3D12Renderer::DrawVideoFrame(const VideoFrame& frame) {
    if (!m_initialized) {
        return shared::Result<void>(shared::ErrorCode::NotInitialized,
                                    "Renderer not initialized");
    }
    if (!frame.valid) {
        return shared::Result<void>(shared::ErrorCode::InvalidArgument,
                                    "Invalid video frame");
    }

    // NOTE: no m_renderMutex lock here on purpose. The only caller
    // (Engine::RenderLoop) invokes this between BeginFrame() and
    // EndFrame(), which already hold the mutex for the whole frame
    // (same reasoning as UpdateTexturePixels()).
    ID3D12Device* dev = m_device.Get();

    // ── Hardware path: decoder GPU surface (zero-copy) ───────────────
    if (frame.sharedHandle != nullptr) {
        if (frame.sharedHandle == m_videoSharedHandle && m_videoTexture) {
            // The decoder recycled the same surface: the SRV from the
            // previous frame is still valid — only dimensions may have
            // been refreshed.
            m_hasVideoFrame = true;
            m_videoWidth    = frame.width;
            m_videoHeight   = frame.height;
            return shared::Result<void>();
        }

        Microsoft::WRL::ComPtr<ID3D12Resource> surface;
        HRESULT hr = dev->OpenSharedHandle(frame.sharedHandle,
                                           IID_PPV_ARGS(&surface));
        if (FAILED(hr)) {
            REX_LOG_ERROR("D3D12", "DrawVideoFrame: OpenSharedHandle failed "
                         "(0x{:08x})", static_cast<unsigned long>(hr));
            if (IsDeviceRemovedHr(hr)) m_deviceRemoved = true;
            return shared::Result<void>(shared::ErrorCode::UnknownError,
                                        "Failed to open shared video surface");
        }

        // SRV at slot 1 (slot 0 is the static-image texture). The shared
        // surface is RGBA8 by contract (VideoColorPipeline output).
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format                  = surface->GetDesc().Format;
        srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels     = 1;

        const UINT srvSize = dev->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu =
            m_srvHeap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += srvSize;
        dev->CreateShaderResourceView(surface.Get(), &srvDesc, cpu);

        // Release the previously opened surface (the decoder recycles
        // handles; opening one per distinct handle stays bounded).
        m_videoTexture.Reset();

        m_videoTexture      = surface;
        m_videoSharedHandle = frame.sharedHandle;
        m_videoSRVGpuHandle = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
        m_videoSRVGpuHandle.ptr += srvSize;
        m_hasVideoFrame = true;
        m_videoWidth    = frame.width;
        m_videoHeight   = frame.height;

        REX_LOG_DEBUG("D3D12", "Video surface attached ({}x{})",
                      frame.width, frame.height);
        return shared::Result<void>();
    }

    // ── Software fallback: CPU pixels ────────────────────────────────
    auto result = UpdateTexturePixels(frame.pixels, frame.width,
                                      frame.height, frame.rowPitch);
    if (result.IsSuccess()) {
        m_hasVideoFrame = true;
        m_videoWidth    = frame.width;
        m_videoHeight   = frame.height;
        // Same SRV as the static path (slot 0).
        m_videoSRVGpuHandle = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    }
    return result;
}

// ─── Frame Rendering ──────────────────────────────────────────────────────

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

    // Draw textured fullscreen quad (active video surface takes precedence)
    if (m_hasTexture || m_hasVideoFrame) {
        m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());
        m_commandList->SetPipelineState(m_pipelineState.Get());
        m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

        D3D12_VERTEX_BUFFER_VIEW vbv = {};
        vbv.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress();
        vbv.SizeInBytes    = sizeof(Vertex) * 4;
        vbv.StrideInBytes  = sizeof(Vertex);
        m_commandList->IASetVertexBuffers(0, 1, &vbv);

        ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
        m_commandList->SetDescriptorHeaps(1, heaps);
        m_commandList->SetGraphicsRootDescriptorTable(
            0, m_hasVideoFrame ? m_videoSRVGpuHandle : m_textureSRVGpuHandle);

        m_commandList->DrawInstanced(4, 1, 0, 0);
    }

    // Transition back to present
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

    // Copy via command list
    auto uploadAlloc = m_device.CreateCommandAllocator();
    if (!uploadAlloc) {
        if (IsDeviceRemovedHr(m_device.GetDeviceRemovedReason())) m_deviceRemoved = true;
        m_texture.Reset();
        return false;
    }

    auto uploadList = m_device.CreateCommandList(uploadAlloc.Get());
    if (!uploadList) {
        if (IsDeviceRemovedHr(m_device.GetDeviceRemovedReason())) m_deviceRemoved = true;
        m_texture.Reset();
        return false;
    }

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

bool D3D12Renderer::CreateRootSignature() {
    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType             = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors        = 1;
    range.BaseShaderRegister    = 0;
    range.RegisterSpace         = 0;

    D3D12_ROOT_PARAMETER param = {};
    param.ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable.NumDescriptorRanges = 1;
    param.DescriptorTable.pDescriptorRanges   = &range;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

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
    desc.NumParameters     = 1;
    desc.pParameters       = &param;
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

bool D3D12Renderer::CreatePipelineState() {
    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, psBlob, errorBlob;

    HRESULT hr = D3DCompile(g_vertexShaderSource, strlen(g_vertexShaderSource),
                nullptr, nullptr, nullptr, "main", "vs_5_0",
                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsBlob, &errorBlob);
    if (FAILED(hr)) {
        REX_LOG_ERROR("D3D12", "Vertex shader compile failed");
        return false;
    }

    hr = D3DCompile(g_pixelShaderSource, strlen(g_pixelShaderSource),
           nullptr, nullptr, nullptr, "main", "ps_5_0",
           D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psBlob, &errorBlob);
    if (FAILED(hr)) {
        REX_LOG_ERROR("D3D12", "Pixel shader compile failed");
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
    psoDesc.PS                        = {psBlob->GetBufferPointer(), psBlob->GetBufferSize()};
    psoDesc.InputLayout               = {inputLayout, 2};
    psoDesc.RasterizerState.FillMode  = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode  = D3D12_CULL_MODE_NONE;
    psoDesc.BlendState.RenderTarget[0].BlendEnable       = TRUE;
    psoDesc.BlendState.RenderTarget[0].SrcBlend          = D3D12_BLEND_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].DestBlend         = D3D12_BLEND_INV_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].BlendOp           = D3D12_BLEND_OP_ADD;
    psoDesc.BlendState.RenderTarget[0].SrcBlendAlpha     = D3D12_BLEND_ONE;
    psoDesc.BlendState.RenderTarget[0].DestBlendAlpha    = D3D12_BLEND_ZERO;
    psoDesc.BlendState.RenderTarget[0].BlendOpAlpha      = D3D12_BLEND_OP_ADD;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.SampleMask              = UINT_MAX;
    psoDesc.PrimitiveTopologyType   = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets        = 1;
    psoDesc.RTVFormats[0]           = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count        = 1;

    m_pipelineState = m_device.CreateGraphicsPipelineState(psoDesc);
    if (!m_pipelineState) {
        REX_LOG_ERROR("D3D12", "CreateGraphicsPipelineState failed");
        return false;
    }

    return true;
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

    const int texW = m_hasVideoFrame ? m_videoWidth : m_texWidth;
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
```

---

## 11. `D:\REX_doc\rex-core\src\graphics\compositor\compositor.h` + `compositor.cpp` — NO CODE CHANGE

Retained as the legacy layer compositor for the worker-window / widget path.
It is still compiled and tested (`render_scheduler.cpp`, `tests/test_compositor.cpp`),
and its `UpdateLayerPixels()` CPU-upload path keeps serving the widget host.

The new video pipeline does **not** route through it: decoded frames go from
`VideoManager` (NEW) straight to `D3D12Renderer::DrawVideoFrame()`. Keep the
two files byte-for-byte unchanged.

---

## 12. `D:\REX_doc\rex-core\src\wallpaper\renderer\VideoRenderer.hpp` + `VideoRenderer.cpp` — REMOVED

Dead code: neither file is referenced by any CMake target or test. It depends
on `PlaybackController`, which section 2 removes from the build.

Its responsibilities move to:
- `VideoManager` + `VideoPlaybackController` + `FrameScheduler` (NEW files) —
  frame production, playback state, play/pause/stop, duration/width/height.
- `D3D12Renderer::DrawVideoFrame()` — GPU-native presentation.

Action: delete both files. No CMakeLists edit is required (they were never in
a source list).

---

## 13. `D:\REX_doc\rex-core\src\wallpaper\engine\EngineFactory.cpp` — NO CODE CHANGE

`CreateWallpaperEngine()` returns `std::make_unique<Engine>()`; the new
`Engine.hpp`/`Engine.cpp` keep the same public interface, so the factory is
untouched. Listed for completeness.

---

## 14. `D:\REX_doc\rex-core\tests\wallpaper\test_engine.cpp` — NO CODE CHANGE

The gtest suite exercises the engine purely through `IWallpaperEngine`, whose
interface is preserved (new fields in `EngineTelemetry` are additive). No edit
required. Listed for completeness.


