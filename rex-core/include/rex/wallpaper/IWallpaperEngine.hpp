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
    // Current-mode refresh rate in Hz (0 = unknown). Populated by
    // Engine::EnumerateMonitors via EnumDisplaySettings (Phase 1).
    int     refreshHz  = 0;
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
    // ─── Per-stage diagnostics (Phase 1) ──────────────────────────────
    double   sourceFps      = 0.0;  // stream frame rate
    double   convertFps     = 0.0;  // ConvertAndSignal+publish rate
    uint64_t schedulerDrops = 0;    // skipped between decode & consume
    double   presentFps     = 0.0;  // Present() rate (== renderFps until
                                    // Phase 3 re-present; then >= renderFps)
    uint64_t repeatedFrames = 0;    // re-presents of the same frame (Phase 3)
    uint64_t queueDepth     = 0;    // published-but-unconsumed frames now
    int      displayHz      = 0;    // current monitor refresh rate (0 = unknown)
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
