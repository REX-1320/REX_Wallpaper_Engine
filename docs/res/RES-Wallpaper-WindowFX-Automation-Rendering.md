# RES-Wallpaper-WindowFX-Automation-Rendering v0.9 (Architectural)

> Depends on: ADR-001, ADR-002, RES-Desktop-Platform, RES-Widget-Platform
> Status: **Architectural outline, not line-by-line implementation.** Detail these
> to full spec depth (matching RES-Widget-Platform's level) at the start of
> Phase 3, once Phase 2 has taught real lessons about the IPC boundary, sandbox
> overhead, and rendering pipeline under load. Writing exhaustive detail for
> these now would be speculative — these modules depend on decisions you
> haven't validated yet.

------------------------------------------------------------------------

## 1. Wallpaper Platform (Phase 3)

### Core responsibilities
- Assign a wallpaper source (static image, video, web page, or "interactive" —
  a special widget-like package with no window chrome) per monitor.
- Multi-monitor synchronization: a single wallpaper spanning monitors vs.
  independent per-monitor wallpapers, user-selectable.
- Audio-reactive layer: subscribe to a system audio-loopback data source
  (reuses the `IDataSource` abstraction from RES-Widget-Platform §2.3 — this is
  why that abstraction was built generically) and expose amplitude/frequency
  bands as bindable values for wallpaper parameters.

### Architecture sketch
- Static/video wallpapers: rendered by the Core compositor directly (no
  process-per-wallpaper needed, unlike widgets — a wallpaper is trusted
  first-party or a simple media file, not arbitrary executable logic, *unless*
  it's an "interactive" wallpaper, in which case it reuses the widget sandbox
  model exactly).
- Web wallpapers: same WebView2/minimal-renderer choice as RES-SDK-Marketplace §2, reused rather than reinvented.
- Open question to resolve in Phase 3 spike: video wallpaper decode — hardware-accelerated decode via Media Foundation, budget GPU/CPU cost against widget rendering budget (RES-Testing-Security-Performance) so a video wallpaper doesn't starve widget frame time.

### Data model sketch
```json
{ "monitorId": "DISPLAY1", "type": "video", "sourcePath": "...", "audioReactive": true, "loop": true }
```

------------------------------------------------------------------------

## 2. WindowFX (Phase 3)

### Core responsibilities
- Apply visual effects (blur, acrylic, Mica, custom shadows) to application
  windows via DWM API composition, plus REX-authored animation/transition rules.
- Window rules engine: match windows by process name/title/class, apply
  effect + behavior rules (always-on-top, opacity, snap behavior).
- Per-app profiles: rule sets scoped to specific applications, switchable per
  Desktop Profile (RES-Desktop-Platform §4).

### Architecture sketch
- Lives primarily in Core (C++) — DWM composition APIs (`DwmSetWindowAttribute`,
  `DwmExtendFrameIntoClientArea`) are native Win32, no benefit to a managed wrapper here.
- Window rule matching/storage lives in Shell (C#) — rule evaluation is not
  performance-critical, belongs where iteration is fast.
- Open question for Phase 3 spike: how much can be achieved via public DWM
  APIs vs. requiring more invasive hooking (which raises stability/compatibility
  risk with Windows updates) — resolve this before committing to an effect list.

------------------------------------------------------------------------

## 3. Automation Engine (Phase 4)

### Core responsibilities
- Workflow engine: trigger → condition(s) → action(s), with scheduling and history/logging.
- Event bus: the same bus that widgets and window rules publish/subscribe to
  (Vision §5 "one system, not three") — this is the connective tissue of the
  whole platform, not a bolt-on feature.
- Visual flow editor (node-based UI in the Shell).

### Architecture sketch
- Event bus lives in Shell (C#), in-process pub/sub (e.g. a lightweight
  mediator pattern), fed by Core events over the IPC control channel (ADR-002)
  and by Widget Platform lifecycle events.
- Triggers (examples): time-based (schedule), system event (battery level,
  network change), widget-originated (button click inside a widget wired to
  `window.rex.automation.trigger(eventName)` — a new SDK surface addition for
  Phase 4), profile-switch.
- Actions (examples): switch profile, change wallpaper, show notification, run
  external command (**explicitly requires its own permission gate** — arbitrary
  command execution is a significant trust boundary, treat with the same
  rigor as RES-SDK-Marketplace §5, not as an afterthought).
- History/logging: append-only local log (JSON Lines), rotated, surfaced in a
  diagnostics view — needed for a solo dev to debug "why did my automation not fire" reports without remote access to the user's machine.

### Data model sketch
```json
{
  "flowId": "focus-mode",
  "trigger": { "type": "schedule", "cron": "0 9 * * MON-FRI" },
  "conditions": [{ "type": "profileActive", "not": "meeting-mode" }],
  "actions": [{ "type": "switchProfile", "profileId": "work-mode" }]
}
```

------------------------------------------------------------------------

## 4. Rendering Layer (Cross-cutting, hardens through Phase 3)

### Core responsibilities
- GPU compositor (Direct3D 12) — layer composition of widgets, wallpaper, effects.
- Animation engine — timing/easing/interpolation shared by widgets, wallpaper transitions, and WindowFX.
- Render scheduler — enforces the per-widget frame budget (RES-Widget-Platform §4, RES-Testing-Security-Performance).

### Architecture sketch
- Layer model: each renderable (widget texture, wallpaper, effect overlay) is a
  layer with a z-order, transform, and opacity — composited in a single pass
  per frame where possible to minimize GPU state changes.
- Render scheduler is the enforcement point for the "one bad plugin can't tank
  the desktop" principle (Vision §5) — this component is disproportionately
  important relative to its size and deserves the most test coverage of
  anything in the Core.
- Full spec (buffer management, exact D3D12 pipeline state objects, frame
  pacing algorithm) to be written at Phase 3 start, informed by actual
  profiling data from Phase 1's widget rendering under real widget counts.

------------------------------------------------------------------------

## 5. Why These Four Stay at Outline Level For Now

Writing exhaustive line-by-line specs here today would mean guessing at:
- Actual per-widget/per-wallpaper GPU cost (unmeasured until Phase 1/3 spikes run)
- Whether WebView2's overhead is acceptable at scale (unresolved open question, §1)
- Whether public DWM APIs suffice for the desired WindowFX effect list (unresolved, §2)
- What automation trigger/action types real usage actually demands (no usage data yet)

Locking detailed specs before these are known risks the exact failure mode
Vision §10 warns against: parallel half-finished modules built on wrong
assumptions. Revisit this document at the start of Phase 3 and expand each
section to RES-Widget-Platform's level of detail, informed by what Phases 0–2 taught you.
