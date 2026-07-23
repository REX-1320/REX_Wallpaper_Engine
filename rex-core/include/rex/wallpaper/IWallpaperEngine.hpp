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

    // ─── Audio ───────────────────────────────────────────────────────────
    virtual shared::Result<void> EnableAudioReactive(bool enable) = 0;
    virtual shared::Result<void> SetAudioSensitivity(float sensitivity) = 0;

    // ─── Debug ────────────────────────────────────────────────────────────
    virtual void DumpStats() const = 0;
};

// ─── Factory ───────────────────────────────────────────────────────────────

std::unique_ptr<IWallpaperEngine> CreateWallpaperEngine();

} // namespace rex::wallpaper
