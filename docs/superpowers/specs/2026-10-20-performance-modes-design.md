# REX Performance Modes — Design Specification

**Status:** approved with clarifications (2026-10-20) · **Implementation:** not started — see
`docs/superpowers/plans/2026-10-20-performance-modes.md`.

**Phase plan:** `docs/superpowers/plans/2026-10-20-performance-modes.md`.

---

## 1. Intent

REX offers three wallpaper-scoped performance modes — **Power saving / Balanced / Performance**. The
active mode is chosen from the PC's detected hardware; the user can pin a mode, or edit each mode's
levers individually.

REX never changes the machine's Windows power mode. No elevation, no system-wide side effects, no
Windows power-scheme or slider changes.

**Quality bar:** the mode system must be more mature, more explicit and more honest than any other
wallpaper app's equivalent — every state explainable in one line, no silent restarts, no claim that
was not measured.

---

## 2. State model

### 2.1 Selection (persisted, user-owned)

Exactly four selectable states:

```
auto | power | balanced | performance
```

`pinned` is **derived**: `selection != auto` ⇒ pinned. There is no third "biased but unpinned"
state. Returning to **Auto** restores automatic behaviour; a manual selection pins until then.

### 2.2 Effective mode (derived, never persisted)

```
triggers = { on-battery, energy-saver }            // a set, not a boolean

effective = selection != auto
              ? selection
              : triggers != {}  ? power
              : baseline(tier)
```

* **Fullscreen is NOT a mode trigger.** Fullscreen pausing is an independent runtime policy (§7).
  A covering foreground app does not move the effective mode.
* **Reasons** reported to the app: `on-battery`, `energy-saver`. Fullscreen is reported separately
  as a *pause* reason, not a mode reason.
* The downshift clears **only when the trigger set becomes empty** (not when the first trigger
  clears while another is still active).
* **No redundant applications:** levers are re-applied only when the effective mode *or* the applied
  lever set actually changes. Polling must not cause repeated writes or repeated `SET_MODE`.

### 2.3 PC-condition behaviour table

| PC condition | Auto behaviour |
|---|---|
| Desktop, plugged into AC, normal use | stay at the hardware-tier baseline |
| Laptop, running on battery | downshift to Power saving |
| Any PC with Windows Energy Saver active | downshift to Power saving |
| Any PC running a fullscreen game/app | the existing fullscreen-pause policy applies (not a mode change) |
| User explicitly selects Performance | stays Performance, regardless of battery / Energy Saver / fullscreen |

For a desktop the battery condition is simply false — no separate desktop-specific mode exists.
Energy Saver remains a valid trigger on a desktop.

---

## 3. Hardware tier

### 3.1 Inputs

Taken from **the adapter REX actually uses**, not whichever adapter DXGI enumerates first:

* dedicated VRAM (bytes) and the software-adapter flag of that adapter
* physical core count, total RAM
* decode capability **on that adapter**: `ID3D11VideoDevice` profile list **plus**
  `CheckVideoDecoderFormat(profile, DXGI_FORMAT_P010, …)` — a profile GUID alone does not prove the
  complete 10-bit decode path (a decode profile can be listed while the required output format is
  unsupported)

Existing reuse: `CheckHwProfileSupport` and the DXVA GUID table in
`rex-core/src/wallpaper/decoder/FfmpegVideoDecoder.hpp:197-264`; DXGI adapter enumeration pattern at
`rex-core/src/wallpaper/engine/Engine.cpp:47-58`. `CheckVideoDecoderFormat` is used nowhere today and
is new code.

### 3.2 Formula (exact)

* **Weak** — software adapter **OR** dedicated VRAM **< 4 GB** **OR** (neither AV1 Main 10-bit **nor**
  HEVC Main10 hardware decode supported) **OR** (physical cores **≤ 4** AND total RAM **≤ 8 GB**)
* **Strong** — AV1 Main 10-bit hardware decode **AND** dedicated VRAM **≥ 8 GB** **AND** physical
  cores **≥ 8**
* **Mid** — everything else

**Deliberate decision:** the Weak rule reads *neither…nor* — a machine that lacks only one of the two
codecs is **not** Weak. Making "missing either codec ⇒ Weak" would classify more systems as Weak;
that is a different, deliberate decision and is not taken here.

Baseline mapping: Weak → **power**, Mid → **balanced**, Strong → **performance**.

### 3.3 Authority

The native runtime's latest report is **authoritative**. The app persists `tier` + `detectedAt` for
startup/display context only — a cached tier is never a substitute for detecting the active adapter.

---

## 4. Presets (levers)

Every lever below is **editable per mode** by the user; "Reset to preset" restores the defaults.
Overrides persist per mode.

### 4.1 Live levers (hot-applicable, no restart)

| Lever | Values | Power saving | Balanced | Performance |
|---|---|---|---|---|
| Frame rate cap (video) | off / 60 / 30 / 24 / 15 | 24 | 30 | off |
| Process priority | below normal / normal | below normal | normal | normal |
| Pause on fullscreen app | on / off | on | on | on |
| Audio | keep / mute | keep | keep | keep |

### 4.2 Load-time levers (apply at the next wallpaper load)

| Lever | Values | Power saving | Balanced | Performance |
|---|---|---|---|---|
| HDR output | follow windows / force SDR | force SDR | follow windows | follow windows |
| Video stream resolution | native / lower | lower | native | native |

**"Force HDR" is deliberately not offered.** REX's HDR follows the Windows output state; forcing HDR
on would require changing the display configuration, which is out of scope for a wallpaper app. Only
*Follow Windows* and *Force SDR* exist. If the native pipeline later genuinely supports forcing HDR
without touching Windows settings, it may be added then.

### 4.3 Independent policy (not a mode lever)

On **session lock / display off**: `pause` / `stop (free VRAM)` / `keep running`. This is a separate
app-level setting and is never changed by a mode switch or an auto-downshift.

---

## 5. When settings take effect (authoritative)

| Action | Behaviour |
|---|---|
| Switch modes | Apply eligible live levers immediately; **do not restart playback** |
| Auto-downshift | Apply eligible live levers only |
| Change HDR or video-stream preference | Save the new preference for the **next wallpaper load**; the UI marks it *pending* |
| Load another wallpaper | Apply its effective mode's load-time settings through `INIT` |
| Want the current wallpaper changed immediately | An explicit, separate user action (reload / apply) — never automatic |

An **auto-downshift applies all eligible live levers from the user's Power-saving preset**,
including a custom audio mute, and **excludes** load-time settings and the independent
lock/display-off policy.

---

## 6. Lever semantics and honest limits (measurement-driven)

| Lever | May reasonably affect | Must **not** be assumed |
|---|---|---|
| Frame rate cap | render/present cadence and downstream GPU work | that the decoder decodes fewer frames |
| Lower video-stream resolution | which stream is decoded, and that stream's decode work + decode surfaces | that a single-stream file gains a lower variant |
| Force SDR | the HDR processing path and output resources | that VRAM necessarily drops (formats and resource layouts matter) |
| Below-normal priority | scheduling precedence under contention | that idle CPU% or power consumption falls |
| Pause/stop policy | work and resources retained during obscured or idle states | that a scene `STOP` frees VRAM by itself |

Additional rules:

* The decoder **selects** a stream; it cannot conjure a lower-resolution variant from a file that
  contains only one 4K stream. **"Lower" = prefer a stream whose width *and* height are ≤ 50 % of the
  monitor's.** If the file has no such stream the lever has no effect on decode, and the UI must say
  so rather than imply otherwise.
* **Output downscaling** (render at reduced resolution, upscale) does not exist in the current
  pipeline and is **not** part of this milestone. If it is added later it must be named *Render
  resolution* and must never be claimed to reduce decoder workload or decode-surface memory.
* CPU/GPU utilization is **evidence, not a measurement of watts**. Every user-facing savings claim
  must be phrased in terms of what was actually measured.
* **"Stop (free VRAM)" means the runtime process exits.** A scene `STOP` keeps the process alive and,
  per the measured per-import pin behaviour, keeps the pinned VRAM charged. Implementation reuses the
  existing intentional-stop path (`wallpaperRuntime.stop({ intentional: true })`) and respawns on
  wake / unlock.

---

## 7. Fullscreen policy (independent of the mode state machine)

* The **existing** fullscreen predicate is reused verbatim — covering-window geometry, shell-chrome
  exclusions, and its persistence window (`Engine::CheckFullScreenPause`, `Engine.cpp:1773`).
  No new detector is introduced.
* `coveringForegroundApp` must **never** mean "any foreground window": ordinary windows cover the
  wallpaper constantly. Only a window that covers essentially every pixel of its monitor counts.
* Fullscreen pausing applies in every mode (the presets set it On) and is reported as a **pause
  reason**, separate from the mode reasons.

---

## 8. Protocol changes (additive; `kProtocolVersion` stays 1)

| Command | Payload | Response |
|---|---|---|
| `SET_MODE` | `{ selection, presets }` | `{ selection, effective, pinned, tier, hardware, levers, triggers, reasons[], pending[] }` — applies live levers immediately |
| `GET_MODE` | — | same shape |
| `INIT` | adds `perfSelection`, `perfPresets` | — (applies the profile from the first frame after a spawn) |

* **Ownership:** the app owns the user's selection *and* the preset overrides; it sends the merged preset
  table with every `SET_MODE` / `INIT`. The native side validates the whole payload and applies it
  atomically (a malformed preset rejects the command rather than installing half a profile).
* **Frame-cap composition:** the mode cap is an additional budget, not a replacement —
  `effective = min(nonzero(INIT/user maxFps), nonzero(mode cap))`, `0 = uncapped`. "Performance = off"
  therefore never raises a cap the caller explicitly set, and "Power saving = 24" always lowers it.
* Old messages stay safely handled: unknown fields ignored, unknown commands rejected cleanly.
* The live frame-rate cap must be **race-free** — a dedicated atomic (repo convention:
  `m_loop` / `m_speed` are already `std::atomic`, `Engine.hpp:133-134`), read by the render loop
  (`Engine.cpp:2487,2506,2344` read `m_config.maxFps` unlocked today).
* The mode must be applied on **every** spawn path: interactive apply, boot re-apply, crash
  re-apply, and the supervisor's recovery re-apply (`supervisor.cjs:275`) — otherwise the mode is
  silently lost after a crash.
* The mode must survive a runtime respawn: it is re-sent through `INIT`.

---

## 9. App surface

### 9.1 Persisted state

`app-state.json` gains:

```
performance: {
  selection: 'auto',
  overrides: { power: {}, balanced: {}, performance: {} },
  tier: null,
  detectedAt: null
}
```

`performance` **must** be added to `AppStateManager.mergeWithDefaults`' explicit key list
(`app/electron/app-state-manager.cjs:152-170`) — otherwise a partially-written object silently loses
its other defaults. `useAppState` is per-key, so `performance` is one object key.

### 9.2 IPC

`perf:get` / `perf:set` → forward to the runtime + persist.

**`perf:get` must not spawn a runtime merely to read a persisted value** — `wallpaperRuntime.send()`
spawns the runtime on any command (`wallpaperRuntime.cjs:172`). Answer from persisted state when the
runtime is not alive; ask the native side only when it is.

### 9.3 UI

A **Performance** page in the sidebar hosts:

* the four-way selection radio (Auto / Power saving / Balanced / Performance)
* the **pinned / auto badge**
* detected-hardware readout: GPU, dedicated VRAM, decode coverage (AV1 Main10 / HEVC Main10), tier,
  `detectedAt`
* the effective-mode line with reasons — e.g. `Auto → Power saving — on battery`
* per-mode lever editors, with **pending** badges on load-time levers
* "Reset to preset" per mode

The sidebar's `Performance` entry (icon `Gauge`) was removed in the current UI refactor and is the
natural home for this; `Settings` keeps app-level preferences.

**Pinned-mode transparency (acceptance requirement):** when a mode is pinned the UI must say so
explicitly — `Pinned: Performance — battery and Energy Saver ignored`. A pinned Performance on
battery must never look like a failed power policy.

### 9.4 Tray

Four radio items (Auto / Power saving / Balanced / Performance) → the same IPC.

---

## 10. Verification

### 10.1 Phase 0 — measurement baseline (before any UI work)

For each lever **independently**, on the target machine (RX 6700 XT, 4K 10-bit clip), record CPU%,
GPU engine %, decode fps, present fps, and decode-pool bytes. This establishes what each lever
actually changes; only then may any savings claim be attached to it. The existing evidence already
answers one question: a frame-rate cap is a **presentation** budget, not a decoder throttle
(`CadenceController.hpp:9-13`).

### 10.2 Automated tests (ctest, pure-function style like `test_power_policy.cpp`)

* tier truth table — including the "neither codec" case and the software-adapter case
* effective-mode truth table — selection × trigger set × tier
* preset → lever mapping, and override/reset merging
* defaults merging for `performance` in `mergeWithDefaults`
* race-free read of the live frame-rate atomic

New sources and test files require **explicit** `CMakeLists.txt` entries — `WALLPAPER_SOURCES`
(`CMakeLists.txt:54-69`) and the test list (`:369-411`). There are no globs.

### 10.3 Live tests

* mode switch mid-playback → no black frame, no engine re-init (live levers only)
* HDR / video-stream preference → pending badge; the change lands on the next load
* Auto downshift on battery → effective mode + reasons update
* pinned Performance on battery → stays Performance, and the UI says it is pinned
* "stop (free VRAM)" → dedicated VRAM actually returns, and the wallpaper resumes on wake/unlock

---

## 11. Non-goals

Windows power scheme / power-mode slider · elevation · per-wallpaper or per-monitor modes · output
downscaling / render scale · forcing HDR on · widgets / taskbar / themes.

---

## 12. Sequencing and preconditions

1. **Reconcile the in-flight working tree and establish a green baseline.** The tree currently carries
   a large uncommitted HDR/tone-mapping change set (`D3D12Renderer.cpp` +577, new
   `HdrToneMapping.hpp`, `test_color_pipeline.cpp` +149, pacing research files) and the last known
   scheduler/backpressure state had a failing playback test. Nothing stacks on an unverified base.
2. **Land the two open P3s from the VRAM work first** — format-alternation re-pin (touches
   `FfmpegVideoDecoder.cpp` + `D3D12Renderer.hpp` `kMaxVideoSlots`) and the VEH false-crash blocks.
   The first collides with Phase 4's files. Per `AGENTS.md`, bugs outrank features.
3. Phase 0 measurement, then the phases below.

### Phase outline

| Phase | Deliverable | Primary files |
|---|---|---|
| 0 | Measurement baseline + honest per-lever effect table | measurement report only |
| 1 | Native tier detection + protocol report | new `HardwareTier.hpp`, `HardwareProbe.*`; `FfmpegVideoDecoder.hpp` (reuse matcher, add P010 check) |
| 2 | Mode engine: state machine, live levers, `SET_MODE`/`GET_MODE` | `Engine.hpp/.cpp`; `wallpaper_cli_main.cpp` |
| 3 | App: state, IPC, Performance page, tray, `INIT` plumbing | `app-state-manager.cjs`; `main.cjs`; `tray-manager.cjs`; new `app/src/settings/*` |
| 4 | Load-time levers: force-SDR via `INIT`; lower video-stream preference | `FfmpegVideoDecoder.cpp`; `VideoManager.cpp`; `wallpaper_cli_main.cpp` |
| 5 | Verification + tracker updates | `TODO.md`; `everything.md`; `#project_status.md` |

---

## 13. Review focus (failure modes the spec implies)

1. **Pinned Performance on battery** — must stay Performance, and must be visibly marked pinned.
2. **Trigger set with two active triggers** — downshift clears only when *both* clear.
3. **Polling with no change** — must not re-apply levers or spam `SET_MODE`.
4. **Load-time lever changed while a wallpaper plays** — pending, no restart, no black frame.
5. **Mode lost after a runtime crash + supervisor recovery** — the mode must be re-sent on respawn.
6. **A file with a single 4K stream + "lower" resolution lever** — no decode change, and the UI must
   not imply one.
7. **"Stop (free VRAM)"** — must actually return dedicated VRAM, and the wallpaper must resume on
   wake/unlock.
