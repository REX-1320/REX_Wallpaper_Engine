# RES-API-Reference v1.0

> The concrete, load-bearing interfaces referenced throughout the other RES
> docs, collected in one place. Extend this file as the SDK grows — it should
> always reflect the actual current API, not the aspirational one.

------------------------------------------------------------------------

## 1. IPC Protocol (Core ⇄ Shell) — `proto/rex.proto`

```protobuf
syntax = "proto3";
package rex.ipc;

message Envelope {
  uint64 id = 1;
  uint32 type = 2;
  bytes payload = 3;
  uint64 timestamp_ms = 4;
}

// Control channel messages
message PingCommand { string message = 1; }
message PongResponse { string message = 1; }

message SetWidgetTransform {
  string instance_id = 1;
  int32 x = 2;
  int32 y = 3;
  int32 width = 4;
  int32 height = 5;
  int32 z_order = 6;
}

message RegisterWidgetSurface {
  string instance_id = 1;
  uint64 shared_memory_handle = 2; // Win32 handle, marshalled cross-process
  int32 width = 3;
  int32 height = 4;
}

message WidgetCrashed {
  string instance_id = 1;
  string reason = 2;
}

message SetTriangleColor { float r = 1; float g = 2; float b = 3; } // Phase 0 spike only, remove post-spike
```

**Message type registry** (the `type` field in `Envelope`) is a simple
incrementing enum maintained in `proto/message_types.proto` — both C++ and C#
codegen from the same source, this is the actual mechanism that keeps ADR-002's
"stable API" promise real rather than aspirational.

------------------------------------------------------------------------

## 2. Shell-Side Service Interfaces (C#)

```csharp
// Widget Platform (RES-Widget-Platform)
public interface IWidgetHostService
{
    Task<WidgetInstance> LoadWidgetAsync(string packageId, string profileId);
    Task UnloadWidgetAsync(Guid instanceId);
    Task<IReadOnlyList<WidgetInstance>> GetActiveWidgetsAsync();
}

public interface IDataSource
{
    string Key { get; }
    TimeSpan PollInterval { get; }
    Task<DataSourceValue> ReadAsync();
    IObservable<DataSourceValue> Changes { get; }
}

public interface IWidgetLayoutRepository
{
    Task<IReadOnlyList<WidgetInstance>> GetLayoutAsync(string profileId);
    Task SaveLayoutAsync(string profileId, IReadOnlyList<WidgetInstance> layout);
}

// Desktop Platform (RES-Desktop-Platform)
public interface IMonitorTopologyService
{
    IReadOnlyList<MonitorInfo> GetMonitors();
    event EventHandler TopologyChanged;
}

public interface IProfileService
{
    Task<Profile> GetActiveProfileAsync();
    Task SwitchProfileAsync(string profileId);
    Task<Profile> SaveAsNewProfileAsync(string name);
    Task DeleteProfileAsync(string profileId);
}

public interface IDesktopHost // wraps native WorkerW acquisition
{
    Task<bool> InitializeAsync();
    bool IsReady { get; }
    event EventHandler HostLost; // fires if Explorer restarts and the host needs re-acquisition
}

// Cross-cutting
public interface IEventBus
{
    void Publish<T>(T evt) where T : class;
    IDisposable Subscribe<T>(Action<T> handler) where T : class;
}
```

------------------------------------------------------------------------

## 3. Widget SDK Surface (TypeScript, injected as `window.rex`)

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
  // Phase 4 addition (RES-Wallpaper-WindowFX-Automation-Rendering §3):
  automation?: {
    trigger(eventName: string, payload?: unknown): void;
  };
  // Only present if manifest declares permissions.network = true
  network?: {
    fetch(url: string, options?: RequestInit): Promise<Response>;
  };
}

type DataSourceValue = { key: string; value: unknown; timestamp: number; error?: string };
type Unsubscribe = () => void;
```

------------------------------------------------------------------------

## 4. CLI Command Reference

| Command | Purpose | Phase |
|---|---|---|
| `rex-cli init <name>` | Scaffold new widget project | 2 |
| `rex-cli dev` | Local dev server + hot reload into running REX | 2 |
| `rex-cli validate` | Manifest schema + permission lint | 2 |
| `rex-cli package` | Produce `.rexwidget` package | 2 |
| `rex-cli publish` | Submit to marketplace review queue | 5 |

------------------------------------------------------------------------

## 5. File Format Quick Reference

| File | Format | Owning doc |
|---|---|---|
| `manifest.json` | JSON, versioned schema | RES-Widget-Platform §2.1 |
| Profile document | JSON, versioned schema | RES-Desktop-Platform §4 |
| Automation flow | JSON, versioned schema | RES-Wallpaper-WindowFX-Automation-Rendering §3 |
| `.rexwidget` package | Zip (manifest + assets), Ed25519-signed | RES-SDK-Marketplace §6 |
| IPC messages | Protobuf, length-prefixed over named pipe | ADR-002, §1 above |

------------------------------------------------------------------------

## 6. Maintenance Note

This document is a living reference, not a spec of record — when a module RES
doc's interface changes, update this file in the same commit. If this file and
a module doc ever disagree, the module doc is authoritative and this file is stale.
