# RES-Widget-Platform v1.0

> Depends on: ADR-001, ADR-002, ADR-003, ADR-004
> Owns: widget lifecycle, rendering contract, data source abstraction, SDK surface for widget authors

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
