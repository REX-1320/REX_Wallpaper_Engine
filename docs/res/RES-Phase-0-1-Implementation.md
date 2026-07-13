# RES-Phase-0-1-Implementation v1.0

> The only document in this package meant to be followed literally, in order,
> starting Monday. Everything else is reference; this is the task list.

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

## Phase 1: Widget Platform MVP (Target: 8–12 weeks)

**Goal:** ship something you personally use daily instead of Rainmeter/nothing.

### Milestone 1.1 — Widget process lifecycle (2–3 weeks)
- Implement `WidgetInstance` model + `WidgetLifecycleState` machine (RES-Widget-Platform §2.2, §3) in the Shell.
- Implement manifest loading + validation (RES-Widget-Platform §2.1).
- Wire widget process spawn through the Phase 0 sandbox proof-of-concept, generalized.
- **Deliverable:** you can install a "hello world" widget package and see its process appear in Task Manager, correctly sandboxed.

### Milestone 1.2 — Rendering pipeline (2–3 weeks)
- Choose and integrate the widget rendering approach (RES-SDK-Marketplace §2 — WebView2 recommended starting point).
- Implement shared-memory texture handoff from widget process → Core compositor (RES-Widget-Platform §4).
- Extend Core compositor to composite an arbitrary number of positioned/sized textures, not just one triangle.
- **Deliverable:** the "hello world" widget's actual HTML content renders on the desktop, positioned and sized correctly.

### Milestone 1.3 — Interaction (1–2 weeks)
- Drag, resize, lock, pin (Vision §5 Widget Platform bullets) — mouse-hit-testing against widget bounds in the Shell, position updates pushed to Core over IPC.
- **Deliverable:** you can drag a widget around your actual desktop and it stays where you put it.

### Milestone 1.4 — Data sources (1–2 weeks)
- Implement `IDataSource` (RES-Widget-Platform §2.3) with first-party sources: `system.cpu`, `system.ram`, `clock`.
- Wire the `window.rex.dataSource` SDK surface (RES-Widget-Platform §5) end-to-end.
- **Deliverable:** a real system-monitor widget showing live CPU/RAM.

### Milestone 1.5 — Persistence (1 week)
- Implement `IWidgetLayoutRepository` (RES-Widget-Platform §6) against a single default profile (full multi-profile switching is Phase 3/RES-Desktop-Platform territory — for Phase 1, one persisted layout is enough).
- **Deliverable:** close and reopen REX, your widgets are exactly where you left them.

### Milestone 1.6 — First-party widget set + polish (1–2 weeks)
- Build 3–5 first-party widgets (clock, system monitor, weather, media session) — this is SDK dogfooding as much as feature delivery; every rough edge you hit here is an edge a third-party author will hit later.
- Basic theme support (a widget can reference a small set of REX-provided color tokens rather than hardcoding colors).

**Phase 1 exit criteria (from Vision §11):** *"you personally use it daily instead of Rainmeter/nothing."* This is a binary, honest check — don't move to Phase 2 planning until this is true for at least a couple of consecutive weeks.

------------------------------------------------------------------------

## What Explicitly Does NOT Belong in Phase 0–1

- Marketplace, publishing, third-party review (Phase 5)
- Multi-profile switching, scenes (Phase 3, though the data model in RES-Desktop-Platform §4 should be *designed* now even if not built, so Milestone 1.5's single-profile persistence format doesn't need a breaking migration later)
- Wallpaper, WindowFX, Automation (Phases 3–4)
- Cloud sync, telemetry backend (post-MVP)

Resist pulling any of these forward. The Phase 1 exit criteria is achievable
without them, and pulling them in early is exactly the "parallel half-finished
modules" failure mode the Vision doc warns about (§10, §19).
