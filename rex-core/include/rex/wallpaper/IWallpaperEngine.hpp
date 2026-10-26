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
    // Requested audio output layout: "auto" (match source, capped by the
    // endpoint), "mono", "stereo", "quad", "surround51". The endpoint may
    // negotiate downward; the effective layout is reported back as the
    // user-notification flag on AudioPlayer.
    std::string audioMode = "auto";
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
    // Rendering adapter (DXGI LUID; 0 = unknown). A monitor moved to a
    // different GPU keeps its device name and geometry, so the predicate
    // must compare this too (P1 topology identity).
    uint64_t adapterLuid = 0;
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
    // ─── A/V sync watchdog ──────────────────────────────────────────────
    // The drift is the CHANGE in the audio-vs-video offset (both playheads
    // lead by a fixed amount, so the offset itself is not an error). A sync
    // error is silent by nature, so it has to be observable without ears.
    double   avSyncDriftMs    = 0.0;
    uint64_t avSyncCorrections = 0;  // times the watchdog had to re-seek
    // ─── Scene end ───────────────────────────────────────────────────────
    // True when the PRESENTED frames reached the end of the scene's
    // content (the video played to its end). Latched by the playback
    // controller and cleared by every seek/scene load, so it reports ONE
    // scene end per latch. The app's playlist engine needs this fact for
    // its "when the current ends" change modes.
    bool     sceneEnded = false;
    // ─── Per-stage diagnostics (Phase 1) ──────────────────────────────
    double   sourceFps      = 0.0;  // stream frame rate
    double   convertFps     = 0.0;  // ConvertAndSignal+publish rate
    uint64_t schedulerDrops = 0;
    // Drops per second over the telemetry window. The cumulative value above
    // never resets and reads alarming long after the drops have stopped.
    double   schedulerDropsPerSec = 0.0;
    double   schedulerDropsPeak = 0.0;    // skipped between decode & consume
    uint64_t loopWraps       = 0;    // PTS wrap/seek events (loop restart or
                                    // explicit seek): separates wrap churn
                                    // from steady-state playback
    // ─── Per-monitor presents (P2) ──────────────────────────────────
    // Compositor present counts keyed by monitor id (0 when dormant).
    struct MonitorPresentCount { std::string monitorId; uint64_t presents = 0; };
    std::vector<MonitorPresentCount> monitorPresents;
    // ─── Occlusion park (vendor-neutral idle) ────────────────────────
    // True while decode+audio are parked because every present path is
    // DWM-occluded (wallpaper hidden). Probe presents continue at ~4 Hz.
    bool occlusionParked = false;
    double   presentFps     = 0.0;  // Present() call rate, trailing ~1 s
                                    // window (unique + cadence re-presents;
                                    // use effectiveVideoFps for unique)
    uint64_t repeatedFrames = 0;    // re-presents of the same frame (Phase 3)
    uint64_t queueDepth     = 0;    // published-but-unconsumed frames now
    int      displayHz      = 0;    // current monitor refresh rate (0 = unknown)
    double   pacingMs       = 0.0;  // live decode-pacing interval
    // ─── Present-interval distribution (jitter) ─────────────────────────
    // presentFps above is a MEAN over the sampling window, and a mean cannot
    // express jitter — a pipeline alternating 37.5/43.75 ms and one locked at
    // 41.667 ms report the identical presentFps. These carry the shape of the
    // distribution, which is what stutter actually is.
    double   presentGapP50Ms    = 0.0;
    double   presentGapP95Ms    = 0.0;
    double   presentGapP99Ms    = 0.0;
    double   presentGapMaxMs    = 0.0;
    double   presentGapJitterMs = 0.0;  // stddev of the interval
    // ─── Per-phase work attribution (last frame + rolling means) ────────
    // "Successful Present() and a healthy average FPS do not establish when a
    // frame actually appeared on screen" — these split the frame so a stall can
    // be attributed to wake / latency-wait / draw+present instead of guessed.
    double   phaseBeginMeanMs = 0.0;   // wake -> BeginFrame
    double   phaseDrawMeanMs  = 0.0;   // draw + RenderFrame + Present
    double   phaseWorkLastMs  = 0.0;   // whole frame, last frame
    double   phaseWaitLastMs  = 0.0;   // frame-latency waitable, last frame
    uint64_t phaseFrameIndex  = 0;
    // Packets abandoned because avcodec_send_packet stayed at EAGAIN for the
    // whole retry budget. Zero is healthy; non-zero means the decoder refused
    // input and a frame may have been lost (send/receive contract).
    uint64_t sendEagainStalls = 0;
    // Driver hardware decoder profile count (0 = no hardware decode).
    uint32_t hwDecoderProfiles = 0;
    // The path the CURRENT stream actually took. hwDecoderProfiles is only a
    // capability count: a driver can advertise profiles and still decode this
    // file in software (chroma gate, profile GUID, or a get_format demotion).
    bool hardwareAccelerated = false;
    // ─── Display cadence, sampled from DXGI (not from our own submits) ───
    // dwellVblanks = refreshes the last presented frame occupied;
    // dwellP50/P95/P99 are over the ring. For a source rate that is not an
    // integer multiple of the refresh rate (24 fps on 160 Hz => 6.667 vblanks)
    // these MUST split between 6 and 7 — that split IS the judder, measured at
    // the panel rather than inferred from submit timing.
    double   dwellVblanks = 0.0;
    // Phase B2 diagnostic: DXGI vblank lattice pitch measured from
    // (dSyncQPCTime / dSyncRefreshCount), plus the last SyncRefreshCount.
    // DIAGNOSTIC ONLY - the pacing servo steers on RATE, not phase.
    double   syncPeriodMs = 0.0;
    uint32_t syncRefreshCount = 0;
    bool     syncPeriodValid = false;
    double   dwellP50     = 0.0;
    double   dwellP95     = 0.0;
    double   dwellP99     = 0.0;
    // True once a full ring of samples exists.
    bool     cadenceValid = false;
    // GPU-pressure present-rate divisor: 1 = normal, 2/4 = degraded.
    uint32_t pressureScale = 1;
    double   pressureWaitMeanMs = 0.0;
    // Phase D decode-ahead reserve: MEASURED depth of already-decoded,
    // not-yet-presented video (pool-capped - often below the 200 ms target),
    // the actual decode rate relative to source, and reserve underruns.
    double   reserveMs = 0.0;
    double   decodeAheadRate = 1.0;
    uint64_t reserveUnderruns = 0;
    // ─── Pacing servo (Phase B2) ───────────────────────────────────────
    // Closed-loop cancellation of the measured +0.30-0.33 ms per-frame pacing
    // bias (docs/bugs/BASELINE-2026-10-15-phaseA.md). All zero and
    // pacingServoActive=false when the servo is disabled or not yet primed, so
    // "no data" is distinguishable from "correcting nothing".
    double   pacingServoCorrMs    = 0.0;  ///< last correction applied, ms
    double   pacingServoErrorMs   = 0.0;  ///< low-passed interval error, ms
    double   pacingServoIntegralMs = 0.0; ///< leaky-integrator state, ms
    bool     pacingServoActive    = false;

    // ─── Wall-cadence presentation (Phase 3) ──────────────────────────
    double   effectiveVideoFps = 0.0;  // unique video frames presented/s
                                       // (excludes cadence re-presents)
    double   playbackSpeed  = 1.0;  // content rate: decoderFps / sourceFps
                                    // (1.0 = realtime; < 1 = decode-bound)
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

    // P0-2 content-rate check: presented-PTS advance per wall second over a
    // 10 s window (1.0 = realtime). position/playbackSpeed/effectiveVideoFps
    // all read healthy while content runs at the wrong speed (2026-10-15:
    // displayed 2x while reporting 1.000x); this one is derived from the
    // content itself. 0.0 until the first window elapses (or no video).
    double contentRate = 0.0;

    // P2-1 unusable-path latch: the decoder never produced a frame for this
    // file (zero progress past budget, or send-fail streak while
    // never-decoded). The supervisor poison-parks on this; the reason is
    // in runtime.log. False when no video is open.
    bool decodePathUnusable = false;

    // P13 per-stage decode-pipeline percentiles (ms, last 128 frames).
    double decodeMsP50 = 0.0, decodeMsP95 = 0.0, decodeMsP99 = 0.0;
    double vpMsP50     = 0.0, vpMsP95     = 0.0, vpMsP99     = 0.0;
    // Decode pacing servo correction on the last sleep deadline (ms,
    // negative pulls earlier). ~-0.5 steady-state on the reference machine.
    double decodeServoCorrMs = 0.0;

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

    // ─── Frame snapshot ───────────────────────────────────────────────────
    /// Writes the frame currently on the desktop to `pathUtf8` as a PNG —
    /// the still the OS keeps as the wallpaper after the engine exits.
    /// Serviced by the render thread (it owns the renderer); blocks for at
    /// most ~10 s. Returns false when nothing is loaded or the encode
    /// failed. Call before Shutdown().
    virtual bool CaptureFrame(const std::string& pathUtf8) = 0;

    // ─── Telemetry ────────────────────────────────────────────────────────
    /// Snapshot of live allocation counters (decoder/ring/GPU/scenes).
    /// May briefly take the engine lock; safe from any thread.
    virtual void GetTelemetry(EngineTelemetry& out) const = 0;

    /// Best-effort telemetry for watchdogs/health monitors. Returns false
    /// (leaving `out` untouched) when the snapshot would block on the
    /// engine lock, so the caller can skip this interval instead of
    /// stalling. Implementations that cannot take a non-blocking snapshot
    /// return false by default.
    virtual bool TryGetTelemetry(EngineTelemetry& out) const {
        (void)out;
        return false;
    }
};

// ─── Factory ───────────────────────────────────────────────────────────────

std::unique_ptr<IWallpaperEngine> CreateWallpaperEngine();

} // namespace rex::wallpaper
