# RES-Wallpaper-Platform v1.0

> Depends on: ADR-001, ADR-002, ADR-004, RES-Widget-Platform §2.3 (IDataSource), RES-Desktop-Platform §3–4
> Owns: wallpaper source management, multi-monitor sync, audio-reactive binding, interactive wallpaper sandboxing

------------------------------------------------------------------------

## 1. Scope

The Wallpaper Platform assigns and renders a wallpaper source per monitor:
static image, video, web page, or "interactive" (a sandboxed widget-like
package with no window chrome, occupying the full wallpaper layer). It owns
source lifecycle, multi-monitor synchronization policy, and the audio-reactive
data pipeline. It does not own compositing itself — the Core compositor
(RES-Rendering-Layer) treats a resolved wallpaper frame as the lowest z-order layer.

------------------------------------------------------------------------

## 2. Core Concepts & Data Model

### 2.1 Wallpaper Assignment (per monitor, part of a Profile)

```json
{
  "monitorId": "DISPLAY1",
  "type": "video",
  "sourcePath": "C:\\Users\\me\\REXTools\\wallpapers\\rain.mp4",
  "audioReactive": true,
  "loop": true,
  "muted": true,
  "fillMode": "fill"
}
```

`type` ∈ `static | video | web | interactive`. `fillMode` ∈ `fill | fit | stretch | tile | center`.

### 2.2 Runtime Model

```csharp
public record WallpaperAssignment
{
    public string MonitorId { get; init; }
    public WallpaperType Type { get; init; }
    public string SourcePath { get; init; }
    public bool AudioReactive { get; init; }
    public bool Loop { get; init; }
    public bool Muted { get; init; }
    public FillMode FillMode { get; init; }
}

public enum WallpaperType { Static, Video, Web, Interactive }

public interface IWallpaperService
{
    Task AssignAsync(string monitorId, WallpaperAssignment assignment);
    Task<WallpaperAssignment?> GetAssignmentAsync(string monitorId);
    Task ClearAsync(string monitorId);
}
```

### 2.3 Multi-Monitor Synchronization Policy

Three modes, user-selectable per Profile:
- **Independent** — each monitor has its own assignment (default).
- **Spanned** — one source stretched/cropped across the combined bounds of
  selected monitors (computed via `IMonitorTopologyService.GetMonitors()` from
  RES-Desktop-Platform §3, treating the union of bounds as one render target).
- **Mirrored** — same source, same playback position, independently rendered
  per monitor (important for video: playback clock must be shared so monitors
  don't visibly drift out of sync over a long-running video loop).

```csharp
public enum SyncMode { Independent, Spanned, Mirrored }
```

For **Mirrored** video, a single shared decode/playback clock (owned by the
Core) drives all monitor instances — do not run N independent decoders, which
would drift.

------------------------------------------------------------------------

## 3. Source-Type Architecture

### 3.1 Static Image

Loaded once into a GPU texture (Core, D3D12), cached, composited per fill mode.
No process isolation needed — treated as trusted data, same trust level as any
local file the user selects. Supported formats: PNG, JPG, WEBP, BMP (via
WIC — Windows Imaging Component, native, no third-party decode library needed).

### 3.2 Video

- **Decode:** Media Foundation hardware-accelerated decode (Core, C++) — avoids
  CPU-bound software decode, keeps the "lightweight" thesis intact for common
  formats (MP4/H.264, WebM/VP9).
- **Frame delivery:** decoded frames written directly to a D3D12 texture via
  Media Foundation's DXGI interop (`IMFDXGIBuffer`), avoiding a CPU round-trip.
- **GPU budget interaction:** video decode competes with widget rendering for
  GPU time — the render scheduler (RES-Rendering-Layer §3) must account for
  wallpaper decode cost in its frame budget, not just widget cost. This is the
  concrete reason RES-Rendering-Layer §3 exists as a cross-cutting scheduler
  rather than being widget-specific.

> **Current limitation:** One shared decoder per runtime. A monitor assigned a
> different video than the main scene cannot play concurrently; it will fall
> back to a static image load with a warning log. Full per-monitor independent
> video decoding requires a future N-controller architecture.

### 3.3 Web

Same rendering approach as widgets/interactive wallpapers — WebView2 instance
(per RES-SDK-Marketplace §2), rendering to an offscreen surface, composited as
the wallpaper layer. A web wallpaper is **not sandboxed as strictly as a
third-party widget** if it's a REX-authored built-in (e.g., a bundled
"live gradient" wallpaper), but **is** sandboxed identically to a third-party
widget if loaded from a user-supplied URL or a marketplace-distributed package
— trust boundary is "did this come from the marketplace/user input," not "is it a wallpaper."

### 3.4 Interactive (Sandboxed)

Identical process/sandbox model to widgets (ADR-003, RES-Widget-Platform §3),
distinguished only by manifest `type: "wallpaper-interactive"` and by occupying
the full-monitor wallpaper layer instead of an arbitrary widget rect. Reuses
`window.rex` SDK surface (RES-API-Reference §3) — an interactive wallpaper is
architecturally just a widget with fixed full-screen bounds and no window chrome.

------------------------------------------------------------------------

## 4. Audio-Reactive Pipeline

### 4.1 Capture

WASAPI loopback capture (Core, C++) on the default render endpoint — captures
system audio output, not microphone input (privacy-relevant distinction, worth
being explicit about in user-facing copy: "reacts to what your PC is playing," not "listens to you").

### 4.2 Processing

- FFT (real-time, windowed) on the captured buffer → frequency bands.
- Exposed as a first-party `IDataSource` (RES-Widget-Platform §2.3):

```csharp
public sealed class AudioReactiveDataSource : IDataSource
{
    public string Key => "system.audio";
    public TimeSpan PollInterval => TimeSpan.FromMilliseconds(16); // ~60Hz
    // Value shape: { bands: number[8], amplitude: number, beatDetected: bool }
}
```

- `beatDetected` uses a simple onset-detection heuristic (energy-based, not a
  full beat-tracking ML model — sufficient for reactive visual parameters,
  avoids unnecessary complexity/CPU cost for Phase 3 scope).

### 4.3 Binding

Wallpaper/widget authors bind to `system.audio` exactly like any other data
source — no separate "audio API," reinforcing the Vision's data-source
abstraction principle rather than adding a bespoke mechanism.

### 4.4 Privacy & Consent

- Audio capture only initializes when at least one active wallpaper/widget
  declares `dataSources: ["system.audio"]` in its manifest **and** the user has
  granted it — same permission-gate pattern as RES-SDK-Marketplace §5, applied
  to a first-party data source, not just third-party plugins.
- Capture stops immediately when no consumer is subscribed — no idle background listening.

------------------------------------------------------------------------

## 5. Persistence

Wallpaper assignments are part of the Profile document (RES-Desktop-Platform
§4), keyed by `monitorId`, versioned per ADR-004. Large media files themselves
are referenced by path, not embedded — Backup & Restore (RES-Desktop-Platform
§6) already handles the "include assets" export flag for portability.

------------------------------------------------------------------------

## 6. Error Handling

| Failure | Handling |
|---|---|
| Video decode fails (corrupt file / unsupported codec) | Fall back to a solid-color placeholder, log, notify user with specific error, do not crash the compositor |
| WebView2 runtime not installed | Prompt user to install (Evergreen bootstrapper), fall back to static placeholder in the meantime |
| Audio capture device unavailable (no default render endpoint) | `system.audio` data source returns a null/error value via the standard `IDataSource` error path (RES-Widget-Platform §7), consumers degrade gracefully |
| Interactive wallpaper crashes | Same crash handling as a widget (RES-Widget-Platform §3 Crashed state), wallpaper layer reverts to last-known-good static frame, not a black screen |
| Monitor topology changes mid-playback (Spanned mode) | Re-resolve combined bounds, restart the spanned render target; brief visible reset is acceptable, silent failure is not |

------------------------------------------------------------------------

## 7. Testing Requirements

- Unit tests: fill-mode calculation math (fill/fit/stretch/tile/center against varied aspect ratios), sync-mode bounds resolution.
- Integration tests: real video decode + render on a CI Windows runner with GPU access (note: this may require a self-hosted runner with a real GPU — standard GitHub-hosted Windows runners may lack sufficient GPU support for D3D12 video interop; validate this early in Phase 3, don't assume it works).
- Adversarial fixtures: a deliberately malformed video file, a web wallpaper attempting the same sandbox-escape fixtures as RES-Widget-Platform §8 (reused, since the sandbox model is identical).
- Perf test: measure actual GPU frame-time cost of 4K video decode + 5 active widgets simultaneously, validate against RES-Testing-Security-Performance Part C budget — this is the number that resolves the open question left in the original outline draft.
