# RES-Phase-0-1-Implementation v1.1

> The only document in this package meant to be followed literally, in order,
> starting Monday. Everything else is reference; this is the task list.

**Changelog:**
- v1.1 (Phase 1): Expanded each milestone with file-level tasks, protocol details,
  and exact contracts. Added §Phase 1 Dependencies section.

------------------------------------------------------------------------

## Phase 0: Foundation Spike (Target: 1–2 weeks, hard time-box)

**Goal:** prove the riskiest unknowns before writing 10,000 lines on top of
unvalidated assumptions. If any step fails badly, you learn it now, not in month 3.

### Step-by-step

1. **Repo scaffolding**
   - `rex-core/` (CMake, C++20 project)
   - `rex-shell/` (.NET 8, WinUI 3 project)
   - `proto/` (shared `.proto` schema files, per ADR-002)
   - Root-level CI config (GitHub Actions, Windows runner — see RES-Testing-Security-Performance §CI)

2. **IPC bridge proof-of-concept**
   - Define one message type in `.proto`: `PingCommand { string message }` / `PongResponse { string message }`.
   - C++ core: open a named pipe server, echo Ping → Pong.
   - C# shell: connect as client, send Ping, log Pong.
   - **Exit check:** round-trip latency measured and logged (target: sub-millisecond for local named pipes; if you're seeing multi-millisecond latency, investigate before proceeding — this pipe carries every frame-critical message later).

3. **WorkerW acquisition spike**
   - Implement `FindRenderTargetWorkerW()` (RES-Desktop-Platform §2) standalone, no rendering yet.
   - Verify it finds the correct window on your current Windows version.
   - **Exit check:** log the HWND successfully, confirm it persists across an Explorer restart (`taskkill /f /im explorer.exe` then relaunch) without crashing your spike app.

4. **First triangle**
   - C++ core: initialize D3D12 device, create a swap chain targeting the WorkerW-child window from Step 3, render a single colored triangle.
   - **Exit check:** triangle visible on the desktop, behind icons, above wallpaper. This is the single most important visual proof in the whole project.

5. **Bridge the two: shell-controlled triangle**
   - Extend the IPC protocol: `SetTriangleColor { float r, g, b }`.
   - C# shell sends a color-change command; C++ core updates the rendered triangle without restarting.
   - **Exit check:** you can change the triangle's color from a WinUI button click. This proves the full command path: UI → IPC → Core → GPU → visible change.

6. **Widget sandbox proof-of-concept (minimal)**
   - Spawn a second process (`rex-widget-host.exe` stub) from the Shell.
   - Apply a Windows Job Object capping its memory to e.g. 50MB.
   - Deliberately allocate past the cap in a test build, confirm the process is killed rather than the machine hanging.
   - **Exit check:** you've proven the sandbox's core enforcement mechanism (ADR-003) works before building anything on top of it.

**Phase 0 exit criteria (from Vision §11):** *"a widget-shaped box survives explorer.exe restart"* — concretely, this means Steps 3–4 combined and re-verified after an Explorer restart.

**If Phase 0 reveals a blocker** (e.g., WorkerW technique fails on your Windows build, or IPC latency is unacceptable): stop, re-open ADR-001/002, do not proceed to Phase 1 on a broken foundation.

------------------------------------------------------------------------

## Phase 1 Dependencies

Before starting Phase 1, verify these Phase 0 artifacts are complete:

- [ ] `rex-proto` IPC messages for `RegisterWidgetSurface`, `SetWidgetTransform`, `WidgetCrashed` exist in `proto/rex.proto` and `message_types.proto`
- [ ] `Sandbox` class (Job Object + process spawn) is tested and passes `ProcessExceedsMemoryLimitIsKilled`
- [ ] `Compositor` accepts + composites layers (not just clear color) — `SubmitLayer`/`RemoveLayer` tested
- [ ] Shell `IpcClient` can connect to Core and send `SetWidgetTransform`
- [ ] CI pipeline builds both Core and Shell

If any of these are missing, complete them before starting Phase 1.

------------------------------------------------------------------------

## Phase 1: Widget Platform MVP (Target: 8–12 weeks)

**Goal:** ship something you personally use daily instead of Rainmeter/nothing.

### Milestone 1.1 — Widget process lifecycle (2–3 weeks)

**Files to create:**
- `rex-shell/REX.Shell.Services/WidgetManager.cs` — implements `IWidgetHostService`,
  owns `Dictionary<Guid, WidgetInstance>`, spawns/kills widget processes
- `rex-shell/REX.Shell.Services/WidgetProcess.cs` — wraps a single widget process:
  process handle, per-widget named pipe client, crash detection
- `rex-shell/REX.Shell.Services/ManifestValidator.cs` — parses `manifest.json`,
  validates schema per RES-Widget-Platform §2.1, returns `WidgetManifest` record
- `rex-core/src/widget_host_main.cpp` — **replaces** `widget_host.cpp` stub. Full
  main loop: connect to Shell over per-widget named pipe, enter message pump

**Files to modify:**
- `rex-shell/REX.Shell.Services/ServiceInterfaces.cs` — add `record WidgetManifest`
  matching RES-Widget-Platform §2.1 schema
- `rex-shell/REX.Shell/MainWindow.xaml.cs` — add "Install Widget" button that
  opens a file picker for `.rexwidget` packages, calls `WidgetManager.LoadWidgetAsync`

**Per-widget IPC protocol (Shell ↔ Widget Host):**
```
Shell                          Widget Host
  │  CreateNamedPipe(\\.\pipe\REX\widget\{instanceId}\v1)
  │  Spawn process (rex-widget-host --pipe "\\.\pipe\REX\widget\{instanceId}\v1")
  │─────────────────────────────────►
  │                                 ConnectNamedPipe
  │◄─────────────────────────────────
  │  ManifestRequest               │
  │─────────────────────────────────►
  │  ManifestResponse{json}        │
  │◄─────────────────────────────────
  │  SurfaceReady{handle, w, h}    │  (after WebView2 init, M1.2)
  │◄─────────────────────────────────
  │  DataSourcePush{key, value}    │  (M1.4)
  │─────────────────────────────────►
  │  [bidirectional, throughout lifecycle]
```

**Sequence for `LoadWidgetAsync(packageId, profileId)`:**
1. Read `manifest.json` from package directory → `ManifestValidator.Validate()`
2. Create `WidgetInstance` record with `State = Loading`
3. Create per-widget named pipe (Shell as server)
4. Spawn `rex-widget-host.exe --pipe "..."` inside a `Sandbox` Job Object (50MB mem limit)
5. Wait for `ManifestResponse` from widget host
6. Verify manifest matches declared permissions
7. Set `State = Running`, register in `WidgetManager`
8. If pipe disconnects → set `State = Crashed`, show notification, attempt restart (max 3 in 5 min)

**Lifecycle state machine (RES-Widget-Platform §3):**
```
Install → Loading → Running ⇄ Suspended
              ↓            ↓
           Crashed ────→ Stopped
```
Implement as a state machine in `WidgetManager` with explicit transition guards:
- `Running → Suspended`: when widget moves off-screen (monitor disconnected, profile switch)
- `Suspended → Running`: when widget returns to view
- `Running → Crashed`: on pipe disconnect
- `Crashed → Running`: on auto-restart (if within retry budget)

**Error handling matrix:**

| Failure | Handling |
|---|---|
| `manifest.json` missing or invalid schema | Reject install, show specific schema error, no process spawn |
| Process spawn fails (Job Object assign fails) | Mark `Crashed`, log Windows error code, notify user, do not retry |
| Widget process exits/crashes during `Loading` | On pipe disconnect → `Crashed`, auto-restart once, then disable |
| Widget process crashes during `Running` | `Crashed` state, non-blocking notification, offer restart, track crash count |
| 3 crashes within 5 minutes | Auto-disable widget with persistent notice, no silent retry-loop |

**Deliverable:** you install a "hello world" `.rexwidget` package via the Shell UI,
see `rex-widget-host.exe` appear in Task Manager inside a Job Object with a 50MB
memory ceiling, and the widget's lifecycle state is displayed in the Shell.

---

### Milestone 1.2 — Rendering pipeline (2–3 weeks)

**Files to create:**
- `rex-core/src/widget_texture_manager.cpp/.h` — manages shared-texture import for
  widget layers: `OpenSharedHandle`, track `LayerId → ComPtr<ID3D12Resource>`,
  handle invalidation on widget crash
- `rex-core/src/texture_router.cpp/.h` — receives `RegisterWidgetSurface` IPC messages
  and routes to `WidgetTextureManager`

**Files to modify:**
- `rex-core/src/compositor.h/.cpp` — extend `RenderFrame()` to iterate `m_layers`
  and render each visible layer at its `(bounds, zOrder, opacity)`. Add texture
  compositing via full-screen quads per layer (VS+PS from PSO created in Phase 0).
- `rex-core/src/main.cpp` — register handler for `MSG_REGISTER_WIDGET_SURFACE` and
  `MSG_UNREGISTER_WIDGET_SURFACE` IPC messages
- `rex-widget-host` main — add WebView2 initialization (offscreen), render HTML to
  shared texture, send `SurfaceReady` to Shell

**WebView2 integration in widget host:**
```
Widget Host process entry:
1. Parse --pipe argument, connect to Shell's named pipe
2. Create hidden WS_POPUP window (1x1, for message pump)
3. Create CoreWebView2Environment with --disable-gpu-sandbox
4. Create CoreWebView2Controller with IsVisible=false (offscreen)
5. Navigate to widget's index.html (local file path)
6. On WebView2's CompositionControllerCreated event:
   a. Get DCompositionTarget visual
   b. Render to offscreen bitmap → copy to shared D3D12 texture
   c. Send SurfaceReady{instanceId, sharedHandle, width, height} to Shell
7. Enter message pump, handle IPC from Shell (DataSourcePush, Resize commands)
```

**Shared-memory texture handoff (widget host → Core):**

```
Widget Host (C++/WebView2)         Core (C++/D3D12)
        │                                │
        │  Create shared D3D12 texture   │
        │  (CreateCommittedResource       │
        │   with KMT handle)             │
        │                                │
        │  Send RegisterWidgetSurface{   │
        │    instance_id,                │
        │    shared_memory_handle,       │
        │    width, height               │
        │  } ──────────IPC─────────────► │
        │                                │  Import via OpenSharedHandle
        │                                │  Create sRGB SRV
        │                                │  Store in WidgetTextureManager
        │                                │  SubmitLayer() to Compositor
```

**CPU copy path (Phase 1):**
```
WebView2 offscreen bitmap
    → ID3D11Texture2D (staging, CPU-readable)
    → Map/read pixels to system memory
    → ID3D12Device::CreateCommittedResource (upload heap)
    → Write pixels via mapped pointer
    → CloseHandle → share with Core
```
Acceptable for <10 widgets at ~1–3ms per copy. Move to direct GPU-GPU handoff
in Phase 2 if profiling shows this is the bottleneck.

**Compositor extension:**
- `Compositor::RenderFrame()` now does:
  1. Sort layers by zOrder (wallpaper < widgets < overlays, per RES-Rendering-Layer §2)
  2. Clear render target (existing)
  3. For each visible layer with a valid texture:
     - Draw a full-screen quad using the layer's texture SRV
     - Apply opacity as blend factor
  4. Present

**Deliverable:** the "hello world" widget's `index.html` content renders on the
desktop — text, styled elements, and images — at the correct position and size
declared in its manifest.

---

### Milestone 1.3 — Interaction (1–2 weeks)

**Files to create:**
- `rex-shell/REX.Shell.Services/WidgetHitTester.cs` — mouse-hit-test against
  widget bounds in screen space; resolves which widget (if any) is under the cursor
- `rex-shell/REX.Shell/WidgetCanvas.cs` — a transparent overlay window (WS_EX_TRANSPARENT)
  that captures mouse events for drag/resize; positioned above the WorkerW layer

**Files to modify:**
- `rex-shell/REX.Shell.Services/WidgetManager.cs` — add methods:
  - `HandleMouseDown(Guid instanceId, Point screenPos)`
  - `HandleMouseMove(Guid instanceId, Point delta)`
  - `HandleMouseUp(Guid instanceId)`
  - `ToggleLock(Guid instanceId)`
  - `TogglePin(Guid instanceId)`
- `rex-core/src/compositor.cpp` — `PopulateCommandList()` must update vertex
  positions based on widget bounds (m_widgetBounds updated via `SetWidgetTransform` IPC)

**Interaction protocol:**

| User action | Shell detects | IPC message sent | Core action |
|---|---|---|---|
| Click on widget | Hit-test matches widget bounds | — (starts drag in Shell) | — |
| Drag widget | Track mouse delta | `SetWidgetTransform{id, x, y, w, h, z}` (every frame during drag) | Update layer bounds, re-composite |
| Right-click widget | Context menu: Lock/Pin/Close | `MSG_UNREGISTER_WIDGET_SURFACE` (if Close) | Remove layer from compositor |
| Resize (drag corner) | Track delta from drag handle | `SetWidgetTransform{id, newW, newH}` | Update texture import size |

**Hit-testing model:**
```
WidgetCanvas (transparent overlay, WS_EX_TRANSPARENT + WS_EX_LAYERED)
  receives all mouse events on the desktop
      │
      ▼
  WidgetHitTester.HitTest(screenPoint)
      │
      ▼
  Iterates WidgetManager.GetActiveWidgets() reverse-z-order
      │
      ▼
  First widget whose bounds contain point → handle input
  Widgets with Locked=true → pass through (no drag, no resize)
```

**Drag behavior:**
- Snap threshold: 8px from other widget edges (configurable per profile, Phase 3)
- Pin to desktop: widget stays in place when workspace/monitor layout changes
- Locked widgets: cannot be moved, resized, or closed except by unlocking first

**Deliverable:** you can click, drag, resize, lock, and close widgets on your
actual desktop. Widgets stay where you put them — position persists in memory
(disk persistence is M1.5).

---

### Milestone 1.4 — Data sources (1–2 weeks)

**Files to create:**
- `rex-shell/REX.Shell.Services/DataSources/CpuDataSource.cs` — `IDataSource`,
  reads `\\.\HKEY_PERFORMANCE_DATA` or `NtQuerySystemInformation`
- `rex-shell/REX.Shell.Services/DataSources/RamDataSource.cs` — `IDataSource`,
  reads `GlobalMemoryStatusEx`
- `rex-shell/REX.Shell.Services/DataSources/ClockDataSource.cs` — `IDataSource`,
  returns `DateTime.Now` formatted
- `rex-shell/REX.Shell.Services/DataSourceRouter.cs` — registry of all data sources,
  resolves `"system.cpu"` → `CpuDataSource`, manages poll intervals, pushes updates
  to subscribed widget processes via per-widget IPC

**Files to modify:**
- `rex-shell/REX.Shell.Services/ServiceInterfaces.cs` — ensure `DataSourceValue`
  record matches the JS SDK surface contract `{ key, value, timestamp, error? }`
- `rex-widget-host` main — add handler for `DataSourcePush` IPC messages, forward
  to WebView2 JS via `PostWebMessageAsJson` → `window.rex.dataSource` callback

**Data source contract:**

```csharp
// Resolution: DataSourceRouter gets a data source by key
IDataSource ds = _router.Resolve("system.cpu");

// Each data source returns a typed value
public class CpuDataSource : IDataSource {
    public string Key => "system.cpu";
    public TimeSpan PollInterval => TimeSpan.FromSeconds(1);
    // Returns: { percent: 45.2 }
}

public class ClockDataSource : IDataSource {
    public string Key => "clock";
    public TimeSpan PollInterval => TimeSpan.FromSeconds(1);
    // Returns: { iso: "2026-07-13T14:30:00", formatted: "2:30 PM" }
}

public class RamDataSource : IDataSource {
    public string Key => "system.ram";
    public TimeSpan PollInterval => TimeSpan.FromSeconds(2);
    // Returns: { totalGB: 32, usedGB: 12.5, percent: 39.1 }
}
```

**Push flow:**
```
DataSourceRouter.Tick() (runs on a background timer at shortest PollInterval)
    │
    ▼
For each data source:
  ReadAsync() → DataSourceValue
    │
    ▼
For each widget subscribed to this data source:
  IPC: DataSourcePush{key, value} → widget host
    │
    ▼
Widget host → WebView2.CoreWebView2.PostWebMessageAsJson(rawValue)
    │
    ▼
widget script: window.rex.dataSource.subscribe("system.cpu", (v) => { ... })
```

**Widget subscription (requested at widget init, from manifest's `dataBindings`):**
```
Shell reads widget manifest → dataBindings: ["system.cpu", "clock"]
Shell registers this widget's IPC pipe as a subscriber to system.cpu and clock
DataSourceRouter includes this widget in its push list
```

**Deliverable:** open the system-monitor widget (built in M1.6) and see live
CPU/RAM/clock values updating in real time. Data source unsubscription is
automatic on widget unload.

---

### Milestone 1.5 — Persistence (1 week)

**Files to create:**
- `rex-shell/REX.Shell.Services/WidgetLayoutRepository.cs` — implements
  `IWidgetLayoutRepository`, reads/writes JSON file at
  `%LOCALAPPDATA%\REXTools\profiles\default.json`
- `rex-shell/REX.Shell.Services/JsonMigration.cs` — versioned migration functions
  per ADR-004 (`MigrateV1ToV2`, etc.), run on file load

**Files to modify:**
- `rex-shell/REX.Shell.Services/WidgetManager.cs` — call `SaveLayoutAsync` on
  every state change (position move, resize, add, remove); call `LoadLayoutAsync`
  on startup
- `rex-shell/REX.Shell.Services/ServiceInterfaces.cs` — ensure `Profile` and
  `WidgetInstance` records serialize cleanly with `System.Text.Json`

**Persistence format (single profile):**

```json
{
  "schemaVersion": 1,
  "profileId": "default",
  "name": "Default",
  "widgets": [
    {
      "instanceId": "a1b2c3d4-...",
      "packageId": "com.rex.clock",
      "x": 100, "y": 50,
      "width": 240, "height": 160,
      "zOrder": 0,
      "locked": false,
      "pinned": false,
      "state": "Running"
    }
  ],
  "lastModified": "2026-07-13T14:00:00Z"
}
```

**Load sequence on startup:**
1. Shell starts → `WidgetManager.LoadProfileAsync("default")`
2. Read `default.json` from disk → apply migrations if `schemaVersion` differs
3. For each widget in the list: call `LoadWidgetAsync(packageId, profileId)`
4. After all widgets loaded: send `SetWidgetTransform` for each to Core compositor
5. If file missing or corrupt → create new empty default profile (do not fail silently)

**Save triggers:**
- Widget moved/resized: debounced 500ms, then write full layout
- Widget added/removed: immediate write
- Widget locked/pinned toggled: immediate write

**Deliverable:** close REX, reopen it — every widget is exactly where you left it,
at the same size, with the same pinned/locked state.

---

### Milestone 1.6 — First-party widget set + polish (1–2 weeks)

**Files to create:**
- `widgets/com.rex.clock/manifest.json`, `index.html`, `script.js`, `style.css`
- `widgets/com.rex.system-monitor/manifest.json`, `index.html`, `script.js`, `style.css`
- `widgets/com.rex.weather/manifest.json`, `index.html`, `script.js`, `style.css`
- `widgets/com.rex.media-session/manifest.json`, `index.html`, `script.js`, `style.css`
- `rex-shell/REX.Shell/Theme/ThemeService.cs` — provides color tokens (CSS custom
  properties) to widgets: `--rex-bg`, `--rex-text`, `--rex-accent`, `--rex-shadow`
- `rex-shell/REX.Shell/Theme/theme.css` — default theme token values

**Widget design specs are defined in RES-Widget-Platform §9 — refer to that section
for exact data bindings, layout, and interaction model for each widget.**

**Theme contract:**

Widgets reference REX theme tokens instead of hardcoded colors:
```css
/* widget/style.css */
.widget { background: var(--rex-bg, rgba(0,0,0,0.5)); color: var(--rex-text, #fff); }
.accent { color: var(--rex-accent, #60a5fa); }
```

Tokens are injected into the widget's WebView2 page via:
```js
// Injected by Shell on widget load, before index.html runs
window.__REX_THEME = { bg: 'rgba(0,0,0,0.5)', text: '#ffffff', accent: '#60a5fa' };
```

Theme switching (light/dark/custom) is triggered by the Shell and pushed to all
widgets via IPC → `PostWebMessageAsJson({ type: 'theme', tokens: {...} })`.

**Solo-dev dogfooding rule:** each first-party widget must be built using the same
SDK surface (`window.rex.dataSource`, `window.rex.storage`) that a third-party
author will use — no special internal APIs. Every rough edge you hit is a bug in
the SDK that needs fixing before Phase 2.

**Deliverable:** 4 working, styled widgets that replace your Rainmeter desktop.
You personally use them daily for at least 2 consecutive weeks (Phase 1 exit
criterion).

------------------------------------------------------------------------

## Phase 1 Exit Criteria Checklist

- [ ] M1.1: Widget loads, process appears in Task Manager, sandboxed via Job Object
- [ ] M1.2: Widget HTML renders on desktop at correct position/size
- [ ] M1.3: Drag, resize, lock, pin all work with mouse
- [ ] M1.4: System-monitor widget shows live CPU/RAM/clock data
- [ ] M1.5: Widget layout persists across REX restart
- [ ] M1.6: You use REX widgets daily instead of Rainmeter for ≥2 consecutive weeks
- [ ] Performance: idle CPU <2% with 5 widgets, RAM <250MB total, per RES-Testing-Security-Performance Part C
- [ ] No Phase 1 regression: Phase 0 exit criteria still passes (WorkerW + compositor survive explorer restart)

------------------------------------------------------------------------

## What Explicitly Does NOT Belong in Phase 0–1

- Marketplace, publishing, third-party review (Phase 5)
- Multi-profile switching, scenes (Phase 3, though the data model in RES-Desktop-Platform §4 should be *designed* now even if not built, so Milestone 1.5's single-profile persistence format doesn't need a breaking migration later)
- Wallpaper, WindowFX, Automation (Phases 3–4)
- Cloud sync, telemetry backend (post-MVP)

Resist pulling any of these forward. The Phase 1 exit criteria is achievable
without them, and pulling them in early is exactly the "parallel half-finished
modules" failure mode the Vision doc warns about (§10, §19).
