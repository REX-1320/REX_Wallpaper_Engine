# RES-Widget-Platform v1.1

> Depends on: ADR-001, ADR-002, ADR-003, ADR-004
> Owns: widget lifecycle, rendering contract, data source abstraction, SDK surface for widget authors

**Changelog:**
- v1.1 (Phase 1): Added §9 First-Party Widget Specifications with per-widget
  data bindings, layout, and interaction models.

------------------------------------------------------------------------

## 1. Scope

The Widget Platform is responsible for: loading widget packages, spawning
sandboxed widget processes, managing their position/size/z-order on the desktop,
routing data-source updates to widgets, and persisting widget layout state.
It does **not** own rendering itself (see RES-Wallpaper-WindowFX-Automation-Rendering
for the compositor) — a widget renders its own HTML/CSS content inside an
off-screen surface that the Core compositor then composites onto the desktop.

------------------------------------------------------------------------

## 2. Core Concepts & Data Model

### 2.1 Widget Package (on-disk format)

```
my-weather-widget/
  manifest.json       # required
  index.html           # entry point
  style.css
  script.ts / script.js
  icon.png
  assets/
```

**manifest.json schema:**

```json
{
  "schemaVersion": 1,
  "id": "com.author.weather-widget",
  "name": "Weather",
  "version": "1.2.0",
  "author": "author-name",
  "entry": "index.html",
  "defaultSize": { "width": 240, "height": 160 },
  "minSize": { "width": 120, "height": 80 },
  "permissions": {
    "network": false,
    "dataSources": ["weather", "clock"],
    "storage": true
  },
  "dataBindings": ["weather.temp", "weather.condition"]
}
```

### 2.2 Widget Instance (runtime state)

```csharp
public record WidgetInstance
{
    public Guid InstanceId { get; init; }
    public string PackageId { get; init; }
    public Point Position { get; set; }
    public Size Size { get; set; }
    public int ZOrder { get; set; }
    public bool Locked { get; set; }
    public bool Pinned { get; set; }
    public string? GroupId { get; set; }
    public WidgetProcessHandle ProcessHandle { get; init; }
    public WidgetLifecycleState State { get; set; }
}

public enum WidgetLifecycleState
{
    Loading, Running, Suspended, Crashed, Stopped
}
```

### 2.3 Data Source Abstraction

Widgets never read hardware/system data directly — they bind to a named data
source, which the Shell resolves and pushes updates for. This indirection is
what lets one widget be reskinned against different data (per Vision §5).

```csharp
public interface IDataSource
{
    string Key { get; }                      // e.g. "cpu", "weather", "media-session"
    TimeSpan PollInterval { get; }
    Task<DataSourceValue> ReadAsync();
    IObservable<DataSourceValue> Changes { get; } // for push-based sources
}
```

First-party data sources shipped in Phase 1: `system.cpu`, `system.ram`,
`system.disk`, `clock`, `weather` (via a pluggable HTTP provider), `media.session`.
Third-party data sources are themselves a plugin type (Phase 5, see
RES-SDK-Marketplace §4).

------------------------------------------------------------------------

## 3. Widget Lifecycle

```
[Install] → Loading → Running ⇄ Suspended
                 ↓                  ↓
              Crashed ──────────→ Stopped (user removed / disabled)
```

- **Loading:** manifest validated, permission manifest checked against user grants, sandboxed process spawned (ADR-003).
- **Running:** normal operation, subscribed to declared data sources.
- **Suspended:** widget is off-screen (minimized workspace, hidden monitor) — process kept alive but data source subscriptions paused to save CPU.
- **Crashed:** pipe disconnect or job-object termination detected → Shell logs the event, shows a non-blocking "Widget X crashed" notification, offers restart. Three crashes within 5 minutes → auto-disable with a persistent notice, do not silently retry-loop.
- **Stopped:** explicit user removal; process killed, sandbox storage optionally retained per user choice.

------------------------------------------------------------------------

## 4. Rendering Contract

1. Widget process renders its HTML/CSS/TS content to an off-screen texture (via
   an embedded lightweight web-rendering surface — evaluate CEF-lite vs. a
   minimal custom HTML/CSS subset renderer at Phase 1 spike time; full Chromium
   embedding is explicitly rejected as too heavy per ADR-001's performance thesis).
2. Texture handle is shared with the Core compositor via a shared-memory handle
   (Win32 `CreateFileMapping`), **not** pushed over the IPC pipe — pixel data is
   too large for message-passing.
3. Core compositor composites the texture at `(Position, Size, ZOrder)` as
   declared by the Shell's widget instance state.
4. Frame budget: each widget gets an allotted render-time slice (see
   RES-Testing-Security-Performance §Performance SLAs) — exceeding it repeatedly
   triggers a throttle (render at half framerate) before a kill.

------------------------------------------------------------------------

## 5. SDK Surface (Widget-Facing API)

Exposed to widget TypeScript code via an injected global (`window.rex`):

```typescript
interface RexWidgetAPI {
  dataSource: {
    subscribe(key: string, callback: (value: DataSourceValue) => void): Unsubscribe;
    read(key: string): Promise<DataSourceValue>;
  };
  storage: {
    get(key: string): Promise<string | null>;
    set(key: string, value: string): Promise<void>;
  };
  ui: {
    requestResize(width: number, height: number): void;
    onResize(callback: (w: number, h: number) => void): Unsubscribe;
  };
  lifecycle: {
    onSuspend(callback: () => void): void;
    onResume(callback: () => void): void;
  };
}
```

No `fetch`/`XMLHttpRequest` global access unless `permissions.network` is
declared in the manifest and granted by the user — enforced by the sandbox
(ADR-003), not just by convention.

------------------------------------------------------------------------

## 6. Persistence

Widget layout state persists as part of the active **Desktop Profile** (owned by
RES-Desktop-Platform), not independently — a widget's position is meaningless
without the profile/scene context it belongs to. The Widget Platform exposes:

```csharp
public interface IWidgetLayoutRepository
{
    Task<IReadOnlyList<WidgetInstance>> GetLayoutAsync(string profileId);
    Task SaveLayoutAsync(string profileId, IReadOnlyList<WidgetInstance> layout);
}
```

Serialized per ADR-004 (JSON, versioned schema) as part of the profile document.

------------------------------------------------------------------------

## 7. Error Handling

| Failure | Handling |
|---|---|
| Manifest fails validation | Reject install, show specific schema error to user |
| Process spawn fails | Log, mark widget `Crashed`, notify, do not retry automatically |
| Data source unavailable | Widget receives a `null`/error value via callback, not a thrown exception across the IPC boundary |
| Widget exceeds frame budget | Throttle first, kill after sustained violation (see Performance SLAs) |
| Widget requests undeclared permission at runtime | Denied silently at the sandbox layer, logged for diagnostics |

------------------------------------------------------------------------

## 8. Testing Requirements

- Unit tests (xUnit): manifest validation, lifecycle state transitions, layout persistence round-trip.
- Integration tests: spawn a real sandboxed widget process, verify IPC round-trip, verify job-object resource caps actually trigger.
- Fixture widgets: a minimal "hello world" widget and a deliberately-misbehaving widget (infinite loop, memory bomb) as permanent test fixtures for sandbox validation — these must live in the test suite forever, since sandbox regressions are the highest-severity bug class in this module.

------------------------------------------------------------------------

## 9. First-Party Widget Specifications (Phase 1 M1.6)

These are the widgets built to dogfood the SDK. Each spec defines the widget's
data bindings, default dimensions, visual layout, and interaction model. All
widgets use `window.rex.*` SDK surface exclusively — no privileged internal APIs.

### 9.1 Clock (`com.rex.clock`)

| Property | Value |
|---|---|
| Default size | 240 × 120 |
| Min size | 120 × 60 |
| Data bindings | `clock` |
| Permissions | none |

**Data contract (`clock` data source):**
```typescript
{ iso: string;           // "2026-07-13T14:30:00"
  formatted: string;     // "2:30 PM"
  dateFormatted: string; // "Mon, Jul 13"
  hour: number;
  minute: number;
  second: number;
}
```

**Layout:**
```
┌─────────────────────┐
│                     │
│      2:30 PM        │  ← time, large (48px), bold, monospace
│   Mon, Jul 13       │  ← date, smaller (14px), lighter weight
│                     │
└─────────────────────┘
```

**Interaction:**
- Click: toggle between 12h/24h format (persisted via `window.rex.storage`)
- Right-click: context menu (Lock, Close)
- No drag handles — drag from any point on the widget body

**Theme tokens used:** `--rex-text`, `--rex-bg`

---

### 9.2 System Monitor (`com.rex.system-monitor`)

| Property | Value |
|---|---|
| Default size | 260 × 180 |
| Min size | 160 × 120 |
| Data bindings | `system.cpu`, `system.ram` |
| Permissions | none |

**Data contracts:**

```typescript
// system.cpu
{ percent: number;    // 0.0 – 100.0
  perCore: number[];  // per-core percentages
}

// system.ram
{ totalGB: number;
  usedGB: number;
  percent: number;    // 0.0 – 100.0
}
```

**Layout:**
```
┌─────────────────────────┐
│ ⚙ System Monitor        │  ← header, 12px, uppercase label
│                         │
│ CPU ████████░░ 45%      │  ← horizontal bar (accent color)
│     ██░░░░░░░░ 12%      │  ← per-core mini bars (optional, Phase 2)
│ RAM ████████░░ 39%      │  ← horizontal bar (accent color)
│     12.5 / 32 GB        │  ← numeric readout, 11px
│                         │
└─────────────────────────┘
```

**Interaction:**
- Click CPU label: toggle per-core breakdown
- Click RAM label: toggle between used/total and percentage
- Right-click: context menu

**Theme tokens used:** `--rex-text`, `--rex-bg`, `--rex-accent`

**Performance constraint:** `system.cpu` data source polls at 1s interval; widget
must not re-render the entire DOM on every tick — only update the changed text
nodes and bar widths.

---

### 9.3 Weather (`com.rex.weather`)

| Property | Value |
|---|---|
| Default size | 240 × 200 |
| Min size | 160 × 140 |
| Data bindings | `weather` |
| Permissions | `network: true` (calls weather API) |

**Data contract (`weather` data source):**
```typescript
{ temperature: number;      // °C
  condition: string;        // "Clear", "Cloudy", "Rain", etc.
  icon: string;             // emoji or icon key
  humidity: number;         // 0–100
  windSpeed: number;        // km/h
  location: string;         // "San Francisco, CA"
  forecast: Array<{         // next 3 days
    day: string;            // "Mon"
    high: number;
    low: number;
    icon: string;
  }>;
}
```

**Layout:**
```
┌─────────────────────┐
│ ☀️ 24°C             │  ← icon + temperature, large (36px)
│ Clear               │  ← condition text, 14px
│ San Francisco, CA   │  ← location, 11px, muted
│                     │
│ Mon  ☀️ 26/18       │  ← 3-day forecast row
│ Tue  ☁️ 22/15       │
│ Wed  🌧 19/12       │
└─────────────────────┘
```

**Interaction:**
- Click temperature: toggle °C/°F (persisted via `window.rex.storage`)
- Click location: open location picker (sends `SelectLocation` event to Shell via
  `window.rex.automation.trigger` — stubbed in Phase 1, full in Phase 4)
- Right-click: context menu

**Theme tokens used:** `--rex-text`, `--rex-bg`, `--rex-accent`

**Phase 1 note:** weather data source is a stub that returns hardcoded values
(24°C, Clear). The HTTP provider that calls a real weather API will be implemented
once the `network` permission gate is fully wired in Phase 2. The widget itself is
built now so its layout and data binding contract can be validated.

---

### 9.4 Media Session (`com.rex.media-session`)

| Property | Value |
|---|---|
| Default size | 300 × 80 |
| Min size | 200 × 60 |
| Data bindings | `media.session` |
| Permissions | none |

**Data contract (`media.session` data source):**
```typescript
{ state: "playing" | "paused" | "stopped";
  title: string;
  artist: string;
  album: string;
  thumbnail: string;     // base64 small image or URL
  position: number;      // current playback position (seconds)
  duration: number;      // total duration (seconds)
}
```

**Layout:**
```
┌─────────────────────────────────────┐
│ [🖼]  Song Title           ⏸ ▶ ⏭    │  ← 48px thumbnail, title (14px bold)
│       Artist Name                    │  ← artist (12px) + transport controls
│       ████████░░░░ 1:23 / 3:45      │  ← progress bar, time readout (11px)
└─────────────────────────────────────┘
```

**Interaction:**
- Play/Pause button → `window.rex.automation.trigger("mediaPlayPause")` (stubbed)
- Next/Previous buttons → `window.rex.automation.trigger("mediaNext")` / `mediaPrev`
- Click progress bar → seek to position
- Double-click thumbnail: toggle expanded view (larger album art, Phase 2)
- Right-click: context menu

**Theme tokens used:** `--rex-text`, `--rex-bg`, `--rex-accent`

**Phase 1 note:** media session data source reads from Windows `SMTC` (System
Media Transport Controls) via `Windows.Media.Control.GlobalSystemMediaTransportControlsSession`
— this is a WinRT API accessible from C#. Transport control actions are stubbed
in Phase 1 (UI buttons present but non-functional) since they require the
Automation Engine's `RunCommandAction` trust tier (Phase 4).

---

### 9.5 Widget Package Structure

All first-party widgets follow this directory layout (identical to what
`rex-cli init` produces):

```
widgets/com.rex.{name}/
├── manifest.json       # per §2.1 schema
├── index.html          # entry point
├── script.js           # widget logic (uses window.rex.*)
├── style.css           # themed styles
└── icon.png            # 64×64 icon for Shell UI
```

The `manifest.json` must declare exactly the data sources and permissions the
widget uses — no more, no less. This is the SDK dogfooding constraint: if the
clock widget declares `permissions.network = true` when it doesn't use it,
that's a spec violation.

### 9.6 Widget Build Verification Checklist (per widget)

- [ ] Runs in `rex-cli dev` hot-reload mode
- [ ] Subscribes only to declared `dataBindings`
- [ ] Handles data source unavailability gracefully (shows "—" or last known value)
- [ ] Respects `window.rex.lifecycle.onSuspend`/`onResume` (pauses timers/animations)
- [ ] Persists user preferences via `window.rex.storage`
- [ ] No hardcoded colors — uses `var(--rex-*)` CSS custom properties
- [ ] Frame budget compliant: no forced layout thrashing, no infinite animation loops
