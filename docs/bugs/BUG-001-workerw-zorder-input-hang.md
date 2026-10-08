# BUG-001 — Wallpaper jumps above desktop icons on mouse interaction; input hangs ~1s

> Status: **diagnosed (Phase 1 complete) · fix plan ready, not yet implemented**
> Date: 2026-10-06 · Layer: `rex-core` (engine) · Evidence: code + `%TEMP%\REX\workerw_diag.log`
> Systematic-debugging: root cause below; fixes are Phase 2/3 — do not implement without following the plan order.

## Symptom (user report)

After applying a wallpaper, it does not stay inside its WorkerW child window.
On desktop mouse interaction it:

1. jumps out **above the desktop icons**,
2. hangs for ~1 second,
3. restores back to normal,
4. during which **all mouse interactions and keyboard shortcuts are blocked**.

## Environment / live evidence

`%TEMP%\REX\workerw_diag.log` (written at acquire, 2026-10-06 22:42:32):

```
Desktop: 3840x2160 at (0,0)
Progman: 000000000004031E
  WorkerW … : 198x56 visible=0 defView=0 …   (×11 tiny invisible top-level WorkerWs)
No suitable WorkerW found — creating synthetic WorkerW
Synthetic WorkerW: paint layer 00000000000600CA, insertAfter=0000000000090504 (above paint layer)
Selected WorkerW: 00000000002401D6
```

Key facts:

- On this machine **no real wallpaper WorkerW exists** → every acquire takes the
  **synthetic WorkerW** path (`WorkerWManager::FindBlankWorkerW`, line 751–805):
  a `REXSyntheticWorkerW` child of Progman, z-anchored **once** at creation via
  `hWndInsertAfter = GetWindow(paintLayer, GW_HWNDPREV)`.
- The synthetic window is created with **exstyle `0`** — no `WS_EX_NOACTIVATE`,
  no `WS_EX_TRANSPARENT` (line 765–770). Its child render window *does* have
  both (line 839).
- `%TEMP%\REX\runtime.log` is **0 bytes** (logging broken — separate P1 issue;
  the HangWatchdog comment in `wallpaper_cli_main.cpp:157` already notes
  "spdlog async queue frozen" as an observed failure mode).
- `runtime-telemetry.log` shows the engine rendering normally at ~9.2 fps
  (wallpaper itself works; the defect is layer topology + repair behavior).

## As-built topology (thread & window model)

Thread map in **runtime mode** (the path the Electron app uses, `RunRuntime`):

| Thread | Loop | Owns desktop HWNDs? | Pumps messages? |
|---|---|---|---|
| stdin command thread | `while (ReadFrame(frame))` (`wallpaper_cli_main.cpp:707`) | **Yes** — initial `Acquire()` runs here (`engine->Initialize`, line 760) | **No** |
| Monitor thread | `MonitorExplorer` `WaitForSingleObject(hProcess, 3000)` (`WorkerWManager.cpp:523`) | **Yes** — `ReacquireWorkerW` creates windows here (line 454) | **No** |
| Render thread | `Engine::RenderLoop` (`Engine.cpp:1183`) | No | **No** |
| `DesktopWindowThread` | GetMessage pump (`DesktopWindowThread.cpp`) | **Intended owner — never instantiated** | Yes (dead code) |

Supporting facts:

- `DesktopWindowThread.hpp:15-28` documents the invariant verbatim: *"All
  window operations in ProgmanHost / WorkerWManager must be marshalled here via
  Send(). Without this, Explorer's cross-process SendMessage to a window we own
  is queued to the thread that created that window — and if that thread never
  dispatches messages, Explorer blocks forever (AppHangXProcB1)."*
- Grep proves `DesktopWindowThread` is referenced **only by its own files** —
  never constructed. It landed in commit `1ca6195` (WIP recovery point) and was
  never wired in.
- The global `PumpMessages()` (`Windows.cpp:25`) is declared/defined but
  **never called** anywhere.
- The only message pump for engine windows is in the **CLI single-file mode**
  (`wallpaper_cli_main.cpp:2355`, `GetMessageW` loop) — that mode is fine;
  runtime mode has none.

## Root-cause analysis

### RC-1 — Layer ends up above the icons; the synthetic layer then eats input

**Mechanism (confidence: high for the state, medium for the exact Explorer trigger):**

1. The synthetic WorkerW sits in Progman's child z-order between the wallpaper
   paint layer and the icon layer (DefView). The anchor is computed **once**,
   at creation (`WorkerWManager.cpp:786-803`); nothing ever re-anchors it
   later.
2. Desktop mouse interaction triggers Explorer to re-shuffle the desktop layer
   z-order (DefView activation / wallpaper layer refresh — the code itself
   documents this class of event at `WorkerWManager.cpp:547-551`: *"Explorer
   can recreate the desktop window structure while the process stays alive"*).
   After the shuffle our layer sits **at/above DefView**.
3. While above DefView, hit-testing falls through the render window
   (`WS_EX_TRANSPARENT`) onto the **synthetic WorkerW — which has neither
   `WS_EX_TRANSPARENT` nor `WS_EX_NOACTIVATE`** → every desktop click in that
   area is swallowed by our window; DefView never gets it → **icons dead,
   desktop shortcuts dead**. Keyboard dies too because desktop focus never
   reaches DefView. This is exactly the failure the verify code warns about:
   `WorkerWManager.cpp:374-387` — *"video would cover desktop icons and eat
   input"*.
4. The monitor notices within ≤3 s (`VerifyWallpaperLayer` invariant,
   lines 273-391) and repairs (RC-3) → layer re-anchored below DefView →
   input returns. Total visible episode ≈ "hangs for a sec and restores".

*Remaining unknown (needs instrumentation — P1-D): the precise Explorer-side
action that flips the z-order on mouse interaction. Candidates: DefView
re-insertion on desktop activation, wallpaper paint layer respawn re-inserting
siblings, WM_SPAWNWORKERW-equivalent internal shell refresh.*

### RC-2 — The repair itself is destructive, slow, and re-inits the whole engine

`ReacquireWorkerW` (`WorkerWManager.cpp:423-492`) on every detected violation:

1. `DestroyWindow(m_renderWindow)` — throw away a healthy window (line 434-437);
2. `FindBlankWorkerW()` — **`SendMessageTimeoutW(progman, WM_SPAWNWORKERW) ×4`
   with 1000 ms timeout + `Sleep(100)` after each, plus another `Sleep(100)`
   in the resize path** (lines 604-608, 747) ≈ up to **~4.4 s of shell churn**
   — Progman handling 0x052C rebuilds desktop layers *while we're mid-repair*;
3. create + reparent + `VerifyWithRetry(5, 300)` — up to **1.2 s** more sleep
   (line 470);
4. fires the recovery callback → `Engine` sets `m_recoveryNeeded` →
   `RenderLoop` runs `HandleDeviceLoss()` → **`ReinitializeRenderer`**
   (`Engine.cpp:230`) — tears down video backend + D3D12 device + swapchain
   and rebuilds everything. For a z-order-only violation this is massive
   overkill and is the visible "restore" blink.

Worse, a **failed** repair leaves `m_acquired = false`, and the monitor then
retries the **entire** cycle **every ~3 seconds, forever**
(`WorkerWManager.cpp:556-579`: `else { needsRepair = true; }`) — persistent
shell churn (4× 0x052C + sleeps + window destroy/create per cycle) whenever the
layer keeps failing verify.

### RC-3 — No message pump on any window-owning thread (shell-freeze amplifier)

Any synchronous message Explorer sends to our windows (`REXRenderWindow`,
`REXSyntheticWorkerW`) is queued to the owning thread (stdin command thread or
monitor thread), which **never dispatches** → Explorer's UI thread blocks →
**entire shell input freezes** until cross-process send timeout (the
AppHangXProcB1 signature the codebase already documents). This turns a
1-second z-order glitch into a hard input hang, and deadlocks *both* sides
when our thread is simultaneously inside a synchronous call into Explorer
(`SendMessageTimeoutW`/`SetWindowPos`/`GetWindowRect` during repair).

The project already fixed **one** vector of this (synthetic window class must
not be `L"WorkerW"` — comment at `WorkerWManager.cpp:753-757`, same
AppHangXProcB1) but left the general one unfixed.

### Symptom → cause map

| Symptom | Cause |
|---|---|
| wallpaper doesn't stay in the WorkerW child | RC-1 (Explorer z-order shuffle, anchor computed once, never re-anchored) |
| jumps above icons on mouse interaction | RC-1 step 2 |
| blocks mouse + keyboard while above | RC-1 step 3 (synthetic exstyle 0 swallows input) |
| hangs for a sec | RC-2 (sleeps + WM_SPAWNWORKERW spam + verify retries) and/or RC-3 (Explorer blocked) |
| restores back | monitor re-acquires/re-anchors (RC-2), then full engine re-init |

### Distinguishing confirmed vs. hypothesis

- **Confirmed (code + live log):** synthetic path active on this machine;
  exstyle 0; anchor computed once; destructive repair with sleeps/0x052C spam;
  3 s retry loop on failed repairs; full engine re-init on recovery callback;
  no message pump on owning threads; `DesktopWindowThread` dead code.
- **Hypothesis (needs instrumentation):** exact Explorer z-order trigger on
  mouse click (H-1); which synchronous messages Explorer sends to our windows
  during the freeze (H-2).

## Fix plan (Phase 2/3 — ordered)

### P0-A — Non-destructive, fast repair (fixes visible jump + input-block duration)

1. On verify failure with live windows: **first try re-anchor only** —
   `SetWindowPos(m_workerW, below-DefView anchor)` + `SetWindowPos(renderWindow,
   HWND_BOTTOM)`, re-verify. Destroy+recreate only if windows are actually
   dead (`IsWindow` false) or re-anchor fails.
2. Fire the recovery callback **only when the render HWND actually changed** —
   a z-order repair must not trigger `ReinitializeRenderer` (no wallpaper blink,
   no device teardown).
3. `FindBlankWorkerW`: make the `WM_SPAWNWORKERW` loop run **only on first
   discovery** (Explorer restarted / never acquired), max 1-2 iterations on
   repair path — never 4× + `Sleep(100)` on a z-order hiccup.
4. Failed-repair retry: exponential backoff (3 s → 6 s → 12 s → cap 30 s)
   instead of every-3s-forever; log each attempt with full z-order dump.

### P0-B — Make the synthetic layer input-safe

Create `REXSyntheticWorkerW` with
`WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW` (still never
`WS_EX_TOPMOST`). Add a verify invariant for it. Result: even in the worst
z-order window, clicks pass through to DefView → symptom degrades from
"input dead" to "cosmetic artifact for ≤1 detect cycle".

### P0-C — Wire `DesktopWindowThread` (eliminates shell-freeze class)

1. Instantiate one `DesktopWindowThread` per `WorkerWManager` (started in
   `Acquire`, stopped in `Release`) — the header already specifies this
   lifecycle.
2. Marshal **all mutating HWND ops** through `Send()`: `FindBlankWorkerW` +
   `CreateRenderWindow` + `ReparentToWorkerW` (Acquire), the destroy/rebuild
   sequence (`ReacquireWorkerW`), `DestroySyntheticWorkerW`, `Release`.
   Read-only queries (`IsWindow`, `GetWindowLongPtr`) may stay inline.
3. Watchdog timeouts: `Send(task, timeoutMs)` already logs on timeout — treat
   timeout as repair failure, not a hang.

### P1-D — Instrumentation to close H-1 / H-2 (before/while doing P0)

1. Temporary `SetWinEventHook(EVENT_OBJECT_REORDER, WINEVENT_OUTOFPROC)` on
   the Progman subtree — log child z-order changes with timestamps; correlate
   with a desktop click.
2. Log every message arriving in `REXRenderWindow::WndProc` /
   synthetic WndProc (thread id + msg) — proves what Explorer sends to our
   pumpless windows (H-2).
3. On verify failure, dump full Progman child chain (handle/class/rect) to the
   diag log instead of only overwriting `workerw_diag.log` at acquire.

### P1-E — Fix `runtime.log` being 0 bytes

spdlog file sink not flushing (or async queue stuck) — currently blocks all
post-mortem debugging. Investigate sink/thread config in
`Logger::Initialize` (comment in `wallpaper_cli_main.cpp:157-159` already
flags frozen-queue as observed).

### P2-F — Faster detection (optional)

Once P0-A lands, violation windows are cheap to repair: poll verify every
1 s instead of 3 s, or trigger immediate verify from the WinEvent hook (P1-D).

## Verification plan (Phase 3)

1. Build: `cmake --build rex-core/build` (+ `ctest`).
2. Manual repro loop: apply wallpaper → click desktop rapidly / hover icons /
   Win+D → confirm: no z-order violation (diag logs show verify OK), no
   wallpaper blink (telemetry `presents` counter never resets), input never
   dead.
3. With P1-D instrumentation active: capture the exact Explorer reorder event
   and messages (closes H-1/H-2); remove or gate behind a debug flag after.
4. Event Viewer: no `AppHangXProcB1` / `SendMessage` hangs attributed to
   rex-core during a 30-min soak.
5. Regression: Explorer restart (kill/restart explorer.exe) still recovers;
   CLI single-file mode still pumps.
6. Close out: check off in `TODO.md`, add to `everything.md` ✅ inventory.

## Tracking

- `TODO.md` 🐛 backlog entry: "Wallpaper z-order jump + input hang (BUG-001)"
- Related backlog: uncommitted `WorkerWManager.cpp/.hpp` WIP (this fix builds
  on that diff — commit together once verified).
