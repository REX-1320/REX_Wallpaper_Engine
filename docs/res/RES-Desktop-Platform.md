# RES-Desktop-Platform v1.0

> Depends on: ADR-001, ADR-002, ADR-004
> Owns: WorkerW desktop host, multi-monitor, profiles, scenes, workspace switching, icon management, backup/restore

------------------------------------------------------------------------

## 1. Scope

The Desktop Platform is the foundation everything else attaches to: it owns the
native desktop-hosting window (WorkerW), monitor topology awareness, and the
**Profile** concept — the top-level persisted unit that bundles widget layout,
wallpaper assignment, and window-effect rules into a single switchable state.

------------------------------------------------------------------------

## 2. WorkerW Host (Core, C++)

Windows renders desktop icons in a `SHELLDLL_DefView` window that's a child of a
`WorkerW` window, created dynamically by Explorer. REX's approach:

1. Send `WM_SPAWN_WORKER` message to the `Progman` window to force Explorer to
   create a `WorkerW` sibling if one doesn't already exist (this is the standard,
   long-used technique — same approach Wallpaper Engine/Rainmeter use).
2. Locate the `WorkerW` that has no `SHELLDLL_DefView` child (the "blank" one) —
   this is REX's rendering surface, sitting behind icons, above the wallpaper.
3. Create REX's own child window inside that `WorkerW` for widget/wallpaper compositing.

```cpp
HWND FindRenderTargetWorkerW() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, nullptr);

    HWND workerw = nullptr;
    EnumWindows([](HWND hwnd, LPARAM lparam) -> BOOL {
        HWND shellView = FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr);
        if (shellView) {
            HWND* result = reinterpret_cast<HWND*>(lparam);
            *result = FindWindowExW(nullptr, hwnd, L"WorkerW", nullptr);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&workerw));

    return workerw;
}
```

**Known fragility:** this technique relies on undocumented Explorer behavior and
has changed across Windows versions historically. **Mitigation:** isolate this
code behind an `IDesktopHost` interface with a version-detection fallback path,
and add it to the Phase 0 spike explicitly — if it breaks on a Windows Insider
build, you need to know in week 1, not after shipping.

------------------------------------------------------------------------

## 3. Multi-Monitor Model

```csharp
public record MonitorInfo
{
    public string DeviceId { get; init; }
    public Rectangle Bounds { get; init; }
    public bool IsPrimary { get; init; }
    public float DpiScale { get; init; }
}

public interface IMonitorTopologyService
{
    IReadOnlyList<MonitorInfo> GetMonitors();
    event EventHandler TopologyChanged; // fires on connect/disconnect/resolution change
}
```

Each widget instance and wallpaper assignment references a `DeviceId`, not an
index — monitor indices shift when displays are connected/disconnected, device
IDs are stable. On `TopologyChanged`, the Shell re-resolves layout: widgets
bound to a now-missing monitor are moved to the primary and flagged, not deleted.

------------------------------------------------------------------------

## 4. Profile / Scene Model

**Profile** = the full desktop state: widget layout + wallpaper assignment per
monitor + window-effect rule set + automation flows active. **Scene** = a named,
saved Profile the user can switch to on demand (manually, on schedule, or via
an automation trigger).

```json
{
  "schemaVersion": 1,
  "profileId": "work-mode",
  "name": "Work Mode",
  "monitors": {
    "DISPLAY1": { "wallpaperId": "calm-gradient", "widgetLayoutRef": "layout-work-1" },
    "DISPLAY2": { "wallpaperId": "todo-board", "widgetLayoutRef": "layout-work-2" }
  },
  "windowRulesRef": "rules-work",
  "automationFlowsActive": ["focus-mode-flow"]
}
```

```csharp
public interface IProfileService
{
    Task<Profile> GetActiveProfileAsync();
    Task SwitchProfileAsync(string profileId); // atomic: old layout torn down, new one spun up
    Task<Profile> SaveAsNewProfileAsync(string name);
    Task DeleteProfileAsync(string profileId);
}
```

**Switching semantics:** profile switches are atomic from the user's perspective
— widgets from the old profile are suspended (not killed, if package overlaps
with new profile, to avoid respawn cost) before new widgets are loaded, to avoid
a flash of empty desktop.

------------------------------------------------------------------------

## 5. Desktop Icon Management

- REX can hide/show native desktop icons per profile (toggle `SHELLDLL_DefView` visibility) — a common power-user want (clean desktop for "focus" scenes).
- Icon position snapshot/restore: read icon positions via `LVM_GETITEMPOSITION` on the `SysListView32` inside `SHELLDLL_DefView`, store per-profile, restore on switch.
- **Non-goal for Phase 1:** custom icon rendering/replacement — this is an Advanced Feature (Vision §17), not core scope.

------------------------------------------------------------------------

## 6. Backup & Restore

- Full export: zip of all profile JSON files + referenced wallpaper/widget asset paths (not copies of large media files by default — export references, with an optional "include assets" flag for portability).
- Import: schema-validated, versioned migration applied (ADR-004), conflicting profile IDs prompt rename-or-overwrite.
- This is deliberately simple (file-based) for Phase 1 — cloud sync is explicitly deferred (Vision §5) until there's a backend to justify it.

------------------------------------------------------------------------

## 7. Error Handling

| Failure | Handling |
|---|---|
| WorkerW not found / Explorer behavior changed | Fallback: render in a borderless topmost-but-behind-icons approximation, log a diagnostic, surface a non-blocking warning banner |
| Monitor disconnected mid-session | Affected widgets suspended + reassigned to primary, profile marked dirty, not auto-saved over the original |
| Profile switch fails partway | Roll back to last-known-good in-memory state; do not persist a half-applied profile as "active" |
| Corrupt profile JSON on load | Quarantine the file (rename `.corrupt`), load default/last-good profile, notify user |

------------------------------------------------------------------------

## 8. Testing Requirements

- Unit tests: profile serialization round-trip, monitor topology resolution logic (mocked topology changes), icon position snapshot/restore logic.
- Integration tests: real WorkerW acquisition on a live Windows session (CI must run on Windows runners, not Linux containers — note for CI pipeline design in RES-Testing-Security-Performance).
- Manual test matrix: Windows 10 vs. 11, single/dual/triple monitor, mixed-DPI setups — WorkerW behavior and DPI scaling are the most likely sources of "works on my machine" bugs in this module.
