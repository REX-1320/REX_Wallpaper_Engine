# RES-WindowFX v1.0

> Depends on: ADR-001, ADR-002, ADR-004, RES-Desktop-Platform §4 (profiles)
> Owns: window visual effects (blur/acrylic/Mica/shadows/animations), window rules engine, per-app profiles

------------------------------------------------------------------------

## 1. Scope

WindowFX applies visual effects and behavioral rules to *other applications'*
windows (not REX's own UI) — blur/acrylic/Mica backgrounds, custom shadows,
transition animations, and rule-based behaviors (always-on-top, opacity,
snap behavior) matched by process/title/class.

------------------------------------------------------------------------

## 2. Core Concepts & Data Model

### 2.1 Window Rule

```json
{
  "ruleId": "rule-001",
  "matcher": {
    "processName": "notepad.exe",
    "titlePattern": null,
    "windowClass": null
  },
  "effects": {
    "backgroundMaterial": "acrylic",
    "cornerRadius": 8,
    "shadow": "soft"
  },
  "behavior": {
    "alwaysOnTop": false,
    "opacity": 0.95,
    "snapEnabled": true
  },
  "priority": 10
}
```

Matchers are evaluated by priority (higher first); first match wins per
property — this allows a general rule ("all apps get soft shadows") plus a
specific override ("but not notepad") without rule authors needing to
understand rule ordering deeply, since **specificity, not just priority order,
resolves conflicts** (a rule matching `processName` beats a rule matching only
`windowClass`, regardless of declared priority, unless priorities explicitly tie-break).

### 2.2 Runtime Model

```csharp
public record WindowRule
{
    public string RuleId { get; init; }
    public WindowMatcher Matcher { get; init; }
    public EffectSettings Effects { get; init; }
    public BehaviorSettings Behavior { get; init; }
    public int Priority { get; init; }
}

public record WindowMatcher
{
    public string? ProcessName { get; init; }
    public string? TitlePattern { get; init; }   // regex
    public string? WindowClass { get; init; }
}

public interface IWindowRuleEngine
{
    Task<WindowRule?> ResolveRuleAsync(WindowInfo window);
    Task ApplyRulesAsync(); // re-evaluates all currently tracked windows, e.g. after a rule edit
}
```

------------------------------------------------------------------------

## 3. Window Detection & Tracking (Shell, C#)

- Use `SetWinEventHook` (via P/Invoke) subscribed to `EVENT_OBJECT_CREATE`,
  `EVENT_OBJECT_DESTROY`, `EVENT_SYSTEM_FOREGROUND` — this is the standard,
  low-overhead mechanism for window lifecycle tracking on Windows (same
  category of technique window-management tools like AltTab/PowerToys use).
- On window create: resolve process name (via `GetWindowThreadProcessId` +
  `OpenProcess`/`QueryFullProcessImageName`), title, and class name; run
  `IWindowRuleEngine.ResolveRuleAsync`; if a rule matches, dispatch an
  `ApplyEffect` IPC command to the Core (effects are rendered/composited
  natively, not from the Shell — see §4).
- Maintain a `Dictionary<HWND, TrackedWindow>` for currently-tracked windows,
  cleaned up on `EVENT_OBJECT_DESTROY`.

------------------------------------------------------------------------

## 4. Effect Rendering (Core, C++)

### 4.1 Blur / Acrylic / Mica

- **Documented path (preferred where sufficient):** `DwmSetWindowAttribute`
  with `DWMWA_SYSTEMBACKDROP_TYPE` (Windows 11 22H2+) directly requests
  Mica/Acrylic/Tabbed backdrop from DWM — no custom compositing needed, most
  stable option, but **only available on Windows 11**, and only affects
  windows REX has some control surface over (this API is designed for an
  app's *own* window; applying to arbitrary third-party windows needs verification).
- **Undocumented path (fallback, Windows 10 + broader compatibility):**
  `SetWindowCompositionAttribute` with `ACCENT_ENABLE_ACRYLICBLURBEHIND` —
  widely used by community tools (this is the same private API various
  Rainmeter skins and older blur utilities rely on). **Explicit risk:** this is
  unofficial API surface that Microsoft has changed across Windows builds
  historically — isolate behind `IEffectRenderer` with version detection, and
  budget maintenance time for this specifically breaking on future Windows
  updates. This is the single highest ongoing-maintenance-cost item in WindowFX.
- **Phase 3 spike requirement:** validate both paths on your actual target
  Windows versions before committing to which is default vs. fallback — do not
  assume the documented path's coverage without testing on a real third-party window.

```cpp
class IEffectRenderer {
public:
    virtual bool ApplyBackdrop(HWND target, BackdropType type) = 0;
    virtual bool SupportsBackdropType(BackdropType type) const = 0;
    virtual ~IEffectRenderer() = default;
};

class Dwm22H2EffectRenderer : public IEffectRenderer { /* DWMWA_SYSTEMBACKDROP_TYPE path */ };
class LegacyAccentEffectRenderer : public IEffectRenderer { /* SetWindowCompositionAttribute path */ };
```

### 4.2 Shadows & Corner Radius

- `DWMWA_WINDOW_CORNER_PREFERENCE` (Windows 11) for corner radius.
- Custom shadow rendering (Core-composited overlay, not a DWM attribute) for
  the "soft"/"sharp" shadow style options that go beyond default DWM shadow —
  rendered as a separate low-priority layer in the Rendering Layer's
  composition pass (RES-Rendering-Layer §2), positioned behind the target window.

### 4.3 Animations

- Window open/close/move transitions use the shared Animation Engine
  (RES-Rendering-Layer §4) — same timing/easing primitives widgets and
  wallpaper transitions use, per the Vision's "one system" principle.
- Applied via a transparent, click-through overlay window animating in sync
  with the real window's `WM_WINDOWPOSCHANGING`/`WM_WINDOWPOSCHANGED` messages
  (hooked via the same `SetWinEventHook` infrastructure from §3), not by
  modifying the target window's actual position mid-animation (which risks
  fighting the target app's own layout logic).

------------------------------------------------------------------------

## 5. Per-App Profiles

Window rule sets are scoped and switchable per Desktop Profile
(RES-Desktop-Platform §4) — `windowRulesRef` in the Profile document points to
a named rule set. Switching profiles triggers `IWindowRuleEngine.ApplyRulesAsync()`
to re-evaluate all currently open windows against the new rule set.

------------------------------------------------------------------------

## 6. Persistence

Window rule sets persist as their own JSON documents (versioned per ADR-004),
referenced by ID from Profile documents — same pattern as widget layouts
(RES-Widget-Platform §6), keeping rule sets reusable/shareable across profiles
without duplication.

------------------------------------------------------------------------

## 7. Error Handling

| Failure | Handling |
|---|---|
| `SetWindowCompositionAttribute` fails or behaves unexpectedly on a Windows build | Log with build number, fall back to no-effect (window renders normally, un-styled) — never leave a window in a broken/invisible state |
| Target window closes mid-animation | Cancel animation overlay cleanly, no orphaned overlay windows left on screen |
| Rule regex (`titlePattern`) is invalid | Reject at save-time with a clear validation error, do not silently fail to match at runtime |
| `SetWinEventHook` registration fails (rare, resource exhaustion) | Retry with backoff, surface a diagnostic if persistent — window tracking silently failing is a "nothing works" bug class, must be loud in diagnostics even if quiet to the end user |

------------------------------------------------------------------------

## 8. Testing Requirements

- Unit tests: rule matcher resolution logic (specificity/priority tie-breaking, §2.1), rule set persistence round-trip.
- Integration tests: apply a rule to a real spawned test window (e.g., launch `notepad.exe` in CI, verify effect application via a screenshot-diff or DWM attribute query) — requires a GUI-capable Windows CI runner, note alongside RES-Wallpaper-Platform §7's similar CI requirement.
- Manual test matrix (this module has the highest compatibility-risk surface in the whole project): Windows 10 21H2, Windows 11 21H2/22H2/23H2+, both documented and undocumented effect paths, across at least 3 common third-party apps (browser, terminal, media player) — track results per Windows build explicitly, this is where "worked in Phase 3, broke in a Windows Update six months later" bugs will originate.
