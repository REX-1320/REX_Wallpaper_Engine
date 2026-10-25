# REX Performance Modes — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Status:** documented 2026-10-20, **not started** — implementation is deferred until the preconditions in Phase 0 are green.

**Goal:** Three wallpaper-scoped performance modes (Power saving / Balanced / Performance) selected by a four-way
control (Auto + the three modes), where the hardware tier sets the Auto baseline, Auto downshifts only on battery or
Windows Energy Saver, and every mode is a user-editable preset of individually understandable levers. No Windows power
changes, no elevation, no playback restarts on a mode switch.

**Architecture:** The app owns the persisted selection + preset overrides and sends the merged preset table with every
`SET_MODE` / `INIT`. The native runtime owns the state machine (`selection × triggers × tier → effective mode`), keeps
the applied levers, applies live levers immediately, and reports `pending[]` for load-time levers that need the next
wallpaper load. Tier detection runs once per runtime spawn against the adapter the D3D12 renderer actually uses.

**Tech Stack:** C++20, DXGI (`IDXGIAdapter4`), D3D11 video decode caps (`ID3D11VideoDevice`,
`CheckVideoDecoderFormat(P010)`), `GetSystemPowerStatus`, gtest/ctest, Electron main process (CommonJS), React + Vite,
`node:test`.

**Spec:** [`docs/superpowers/specs/2026-10-20-performance-modes-design.md`](../specs/2026-10-20-performance-modes-design.md)

---

## Global constraints

1. **Wallpaper-scoped only.** Never call `PowerSetActiveOverlayScheme`, `powrsetactive`, `SetThreadExecutionState`
   for power plans, or anything that changes the Windows power mode/slider. No elevation. Only the runtime process's
   own priority class changes — never Electron's.
2. **No restarts.** A mode switch, an auto-downshift, or a lever edit must never stop, respawn, or re-`INIT` the
   runtime. Load-time levers are *pending* until the next wallpaper load, which is a user action.
3. **No new GPU imports.** `OpenSharedHandle` cost is paid per import and pinned until process exit (measured
   ~180 MiB for a 4K P010 pool). Nothing in this feature may add a second import per frame/session.
4. **Build tree is `rex-core/.build`, not `rex-core/build`.**
   `cmake --build rex-core/.build --config Release` and `ctest --test-dir rex-core/.build -C Release --output-on-failure`
   (offline vcpkg toolchain: `%TEMP%\opencode\vcpkg-offline-toolchain.cmake`).
5. **CMake has no globs.** Every new `.cpp` and every new test executable needs an explicit entry in
   `rex-core/CMakeLists.txt` — sources at `:54-69`, tests at `:369-411`.
6. **`kProtocolVersion` stays 1.** `SET_MODE`, `GET_MODE`, and `INIT.perfSelection` / `INIT.perfPresets` are additive;
   unknown fields are ignored, unknown commands keep failing cleanly.
7. **Do not touch** the colour/tone-mapping shader (`D3D12Renderer.cpp` in-flight work), `app/src/widgets/`, or
   `kMaxVideoSlots` except in Task 0.2 (which owns that constant).
8. **One change per commit**, each commit green on its own gate.
9. **Do not conflate** the existing per-wallpaper registry metadata `wallpaper.performance` (`renderer`,
   `frameRateLimit`, `renderResolution`, `powerMode` — import metadata, read by
   `app/src/wallpapers/inspector/sections/PerformanceSection.jsx`) with the new app-state key `performance`
   (mode selection + presets). Different objects, different owners.

**Frame-cap composition rule.** A video apply already passes `INIT.maxFps` (0 = uncapped; `desktop.js:63` sends 0,
`main.cjs:247` defaults 0). The mode's cap is the *additional* budget:

```
effectiveCap = min(nonzero(userCap), nonzero(modeCap))     // 0 means "no cap"
```

so "Performance = off" never raises a cap the caller explicitly set, and "Power saving = 24" always lowers it.

**Verification gates per task**

| Gate | Command |
|---|---|
| Native build | `cmake --build rex-core/.build --config Release` |
| Native tests | `ctest --test-dir rex-core/.build -C Release --output-on-failure` |
| Main-process syntax | `node --check app/electron/<file>.cjs` |
| App-side unit test | `node --test app/electron/perf-presets.test.cjs` |
| App lint/types | `pnpm lint` · `pnpm typecheck` |

---

## Review focus (the failure modes this plan must prevent)

1. Pinned Performance on battery stays Performance **and** the UI says it is pinned.
2. Two triggers active (`on-battery` + `energy-saver`) → the downshift survives until **both** clear.
3. Polling with no change → zero lever writes, zero `SET_MODE` IPC.
4. Load-time lever edited mid-playback → `pending[]`, no restart, no black frame.
5. Mode lost after a runtime crash + supervisor recovery → re-sent through `INIT` on **every** spawn path.
6. A single-stream 4K file + "Lower" → no decode change, and the UI must not imply one.
7. "Stop (free VRAM)" actually returns dedicated VRAM (that path is pre-existing and must not be regressed).

---

## File map

| Path | New/Mod | Purpose |
|---|---|---|
| `rex-core/src/wallpaper/engine/HardwareTier.hpp` | new | Pure tier formula + decode-support combiner |
| `rex-core/src/wallpaper/engine/HardwareProbe.hpp/.cpp` | new | Adapter/VRAM/decode/cores/RAM/power probe |
| `rex-core/src/wallpaper/engine/PerfMode.hpp` | new | Pure selection × triggers → effective mode + reasons |
| `rex-core/src/wallpaper/engine/PerfPreset.hpp` | new | Lever struct, preset table, override merge |
| `rex-core/src/wallpaper/engine/Engine.hpp/.cpp` | mod | Perf state, atomic cap, poll, live apply |
| `rex-core/src/wallpaper_cli_main.cpp` | mod | `SET_MODE` / `GET_MODE` / `INIT` params |
| `rex-core/tests/wallpaper/test_hardware_tier.cpp` | new | Tier truth table |
| `rex-core/tests/wallpaper/test_perf_mode.cpp` | new | Mode truth table |
| `rex-core/tests/wallpaper/test_perf_preset.cpp` | new | Preset table + merge/reset + fixture contract |
| `rex-core/tests/wallpaper/test_protocol_safety.cpp` | mod | Malformed `SET_MODE` / `INIT.perfPresets` payloads |
| `rex-core/tests/wallpaper/test_decoder_pool.cpp` | mod | 2-generation pool cache (Task 0.2) |
| `rex-core/tests/fixtures/perf-presets.json` | new | Cross-language preset contract |
| `rex-core/CMakeLists.txt` | mod | Explicit source + test entries |
| `app/electron/perf-presets.cjs` | new | Defaults, selection normalisation, override merge |
| `app/electron/perf-presets.test.cjs` | new | `node:test` suite incl. fixture contract |
| `app/electron/app-state-manager.cjs` | mod | `performance` default + merge key |
| `app/electron/main.cjs` | mod | IPC, cache, `INIT` plumbing on all spawn paths |
| `app/electron/tray-manager.cjs` | mod | Four radio items |
| `app/electron/supervisor/runtime-manager.cjs` | mod | `init()` signature passes perf params |
| `app/src/settings/PerformancePage.jsx` | new | The page |
| `app/src/settings/performance.css` | new | Page styles (match existing section styling) |
| `app/src/settings/levers.js` | new | Lever option lists + labels (UI only) |
| `app/src/App.jsx` | mod | Route the Performance page |
| `app/src/components/SidebarLayout.jsx` | mod | Re-add the `Performance` / `Gauge` entry |

---

# Phase 0 — Preconditions (nothing stacks on a red or dirty base)

### Task 0.1: Establish a green baseline

**Files:** none (verification only).

- [ ] `git status --porcelain` — record the in-flight set (HDR/tone-mapping `D3D12Renderer.cpp`, untracked
      `HdrToneMapping.hpp`, `test_color_pipeline.cpp`, pacing research). Commit or shelve it; do not build the feature
      on top of uncommitted engine changes.
- [ ] `cmake --build rex-core/.build --config Release` — clean.
- [ ] `ctest --test-dir rex-core/.build -C Release --output-on-failure` — record the pass count (last green: 804/804).
- [ ] If the known playback/scheduler test is red, fix it **in its own commit before Task 0.2**; do not "fix forward"
      inside a perf-mode commit.

**Done when:** build clean, ctest 100 %, working tree clean, commit hash recorded in the plan's task log.

### Task 0.2: P3 — alternating pool formats re-pin a generation per switch

**Files:** `rex-core/src/wallpaper/decoder/FfmpegVideoDecoder.hpp/.cpp`,
`rex-core/src/graphics/renderer/D3D12Renderer.hpp`, `rex-core/src/graphics/renderer/D3D12Renderer.cpp`.

Fix direction (from the VRAM work, already diagnosed — reproduce before changing anything): cache the last **two** pool
generations in the decoder keyed by `(w, h, format)`, and raise `kMaxVideoSlots` to **≥ 16** so the renderer's import
LRU can hold both generations. A→B→A switches then become cache hits instead of a fresh generation + a fresh per-import
pin (measured 526.5 MB → 918.9 MB for BGRA 3840×1752 → P010 3840×1920).

- [ ] Reproduce: alternate the two test files twice, log pool generation + dedicated VRAM after each switch.
- [ ] Add a decoder-side unit test for the 2-generation cache key/lifetime (extend the existing
      `rex-core/tests/wallpaper/test_decoder_pool.cpp`).
- [ ] Change `kMaxVideoSlots` only after the SRV heap descriptor count is verified (`64`-descriptor heap comment in
      `D3D12Renderer.hpp`) — 16 slots need room for per-format SRVs.
- [ ] Live A/B: two A→B→A cycles, VRAM must stay flat after the first cycle.

**Done when:** VRAM after cycle 2 equals cycle 1 (within noise), tests green, `TODO.md` P3 checked off.

### Task 0.3: P3 — VEH logs fake "crash" blocks on every clean shutdown

**Files:** the VEH/crash-logger translation unit under `rex-core/src` (find with
`Select-String -Path rex-core\src\*.cpp,rex-core\src\**\*.cpp -Pattern 'VEH'`).

- [ ] Reproduce: clean `SHUTDOWN`, then read `%TEMP%\REX\crash.log` — a `=== REX VEH exception ===` block for RPC
      status `0x0000071A` (`RPC_S_*`) must **not** be appended.
- [ ] Filter RPC `RPC_S_*` / `0x71A` (or gate the dumper to second-chance exceptions), keeping genuine first-chance
      AV dumps.
- [ ] Clean shutdown twice + one deliberate AV (poison-file) → no block for the former, one for the latter.

**Done when:** clean shutdowns produce no dumps; a real fault still produces one; `TODO.md` P3 checked off.

---

# Phase 0.5 — Measurement baseline (before any UI or claim)

### Task 1: Per-lever effect table

**Files:** `docs/superpowers/specs/2026-10-20-perf-lever-measurements.md` (new), plus a temporary harness under
`%TEMP%` if the existing verify tools do not already cover it.

**Purpose:** every user-facing savings statement must be measured. This task produces the numbers the UI may quote —
and nothing else may be quoted.

Method: the same 4K 10-bit clip, SDR desktop, one lever at a time, 30 s sample after 5 s warm-up, three runs, median.

| Run | Lever | Record |
|---|---|---|
| A0 | baseline (Balanced preset) | fps, CPU %, GPU 3D/decode %, decode-pool bytes, private bytes |
| A1..A4 | frame cap 60 / 30 / 24 / off | same |
| B1 | below-normal priority | same |
| C1 | force SDR (SDR desktop → expect **no change**; record it) | same |
| D1 | lower stream preference on a multi-stream file | which stream was selected + same |
| D2 | lower stream preference on a single-stream file | expect **no change**; record it |

- [ ] Capture A0–A4, B1, C1, D1, D2.
- [ ] Write the table with the honest conclusion per lever (including "no measurable effect").
- [ ] Cross-check the existing finding that the FPS cap is a presentation budget (`CadenceController.hpp:9-13`) —
      decode fps must **not** be claimed to drop.

**Done when:** the report exists, every row has numbers, and any lever with no measured effect is marked "no effect
measured" rather than removed.

---

# Phase 1 — Hardware tier (pure first, platform second)

### Task 2: Pure tier formula

**Files:** `rex-core/src/wallpaper/engine/HardwareTier.hpp`, `rex-core/tests/wallpaper/test_hardware_tier.cpp`,
`rex-core/CMakeLists.txt`.

**Interface:**

```cpp
namespace rex::wallpaper {

enum class HardwareTier { Weak, Mid, Strong };

struct DecodeSupport {
    bool av1Main10 = false;   // profile present AND P010 output supported
    bool hevcMain10 = false;
};

struct TierInputs {
    bool     softwareAdapter  = false;
    uint64_t dedicatedVramBytes = 0;
    bool     av1Main10 = false;
    bool     hevcMain10 = false;
    unsigned physicalCores = 0;
    uint64_t totalRamBytes = 0;
};

inline constexpr uint64_t kTierVramWeakBytes   = 4ull << 30;
inline constexpr uint64_t kTierVramStrongBytes = 8ull << 30;

HardwareTier ComputeTier(const TierInputs& in) noexcept;
const char*  TierName(HardwareTier t) noexcept;                   // "weak" | "mid" | "strong"
PerfMode     BaselineModeForTier(HardwareTier t) noexcept;        // weak→power, mid→balanced, strong→performance

// A codec counts only when its profile AND its 10-bit output format are supported.
DecodeSupport CombineDecodeSupport(bool av1Profile, bool av1P010,
                                   bool hevcProfile, bool hevcP010) noexcept;
}
```

- [ ] Write `test_hardware_tier.cpp` truth table **first** (it must fail to compile/link):
      software adapter ⇒ Weak; 3.9 GB ⇒ Weak; 4.0 GB ⇒ not Weak; 7.9 GB + AV1 + 8 cores ⇒ Mid; 8 GB + AV1 + 7 cores ⇒
      Mid; no AV1 but HEVC ⇒ **not** Weak (the deliberate "neither/nor"); neither codec ⇒ Weak; 4 cores + 8 GB ⇒ Weak
      (≤ bounds are inclusive); 5 cores + 8 GB ⇒ Mid; `TierName`/`BaselineModeForTier` full mapping;
      `CombineDecodeSupport` four-case table (profile-only never counts).
- [ ] Add the new `.cpp` to the **existing** `rex-wallpaper-tests` source list (`CMakeLists.txt:369-411`);
      `gtest_add_tests(TARGET rex-wallpaper-tests)` at `:449` registers the suite automatically — do not create a
      second test executable.
- [ ] Implement, run, iterate to green.

**Done when:** `ctest -R HardwareTier` green; the "neither codec" case carries a comment explaining it is deliberate.

> Suite names `HardwareTier`, `PerfMode`, `PerfPreset` are fixed by this plan so the `ctest -R` filters used below
> resolve.

### Task 3: Platform probe on the adapter REX uses

**Files:** `rex-core/src/wallpaper/engine/HardwareProbe.hpp/.cpp`, `rex-core/CMakeLists.txt`
(`WALLPAPER_SOURCES` at `:54-69`).

**Interface:**

```cpp
namespace rex::wallpaper {

struct HardwareInfo {
    std::string  activeAdapter;        // description of the renderer's adapter
    uint64_t     dedicatedVramBytes = 0;
    bool         softwareAdapter = false;
    bool         av1Main10 = false;
    bool         hevcMain10 = false;
    unsigned     physicalCores = 0;
    uint64_t     totalRamBytes = 0;
    HardwareTier tier = HardwareTier::Mid;
};

// `device` is the renderer's device (graphics::Device::Adapter(), Device.hpp:43).
HardwareInfo ProbeHardware(const graphics::Device& device);

struct PowerTriggers { bool onBattery = false; bool energySaver = false; };
PowerTriggers ReadPowerTriggers() noexcept;   // GetSystemPowerStatus
}
```

Implementation notes (each is load-bearing):

- Take the adapter from `Device::Adapter()` (`Device.hpp:43`), never `EnumAdapterByGpuPreference(0, …)` — on a hybrid
  laptop that can be the iGPU while REX renders on the dGPU (or the reverse).
- Dedicated VRAM: `DXGI_ADAPTER_DESC1::DedicatedVideoMemory`; software flag: `DXGI_ADAPTER_FLAG_SOFTWARE`.
- Decode caps: enumerate `IDXGIFactory` adapters to find the one whose `AdapterLuid` matches the D3D12 adapter, create
  a short-lived `D3D11CreateDevice` on it, then `ID3D11VideoDevice::GetVideoDecoderProfileCount/GetVideoDecoderProfile`
  and `CheckVideoDecoderFormat(profile, DXGI_FORMAT_P010, &supported)`. Reuse the AV1/HEVC GUID table and matcher logic
  from `FfmpegVideoDecoder.hpp:197-264` (`CheckHwProfileSupport`); factor the matcher into a shared header rather than
  copying GUIDs. Release the probe device before returning.
- Cores: `GetLogicalProcessorInformationEx(RelationProcessorCore, …)` → count entries (physical, not logical).
- RAM: `GlobalMemoryStatusEx().ullTotalPhys`.
- Power: `GetSystemPowerStatus` → `onBattery = ACLineStatus == 0`; `energySaver = SystemStatusFlag == 1`.
  `BatteryFlag == 128` (no battery) must report `onBattery = false`, never "unknown ⇒ battery".
- Probe failures must **not** fail the runtime: default `tier = Mid`, log one line, continue.

- [ ] Add a `--print-hardware` debug flag to `wallpaper_cli_main.cpp` that prints `HardwareInfo` as JSON and exits
      (no engine, no window) — this is the live verification hook and costs ~40 lines.
- [ ] Run it on the dev box: RX 6700 XT must report `av1Main10 = true`, `hevcMain10 = true`, 10 cores (or the actual
      count), tier `strong`.
- [ ] Force a software adapter (or a small VRAM fake in a unit test of `ComputeTier`) → `weak`.

**Done when:** `--print-hardware` reports the real adapter and the same tier the formula gives for the observed inputs.

### Task 4: Report tier through the protocol

**Files:** `rex-core/src/wallpaper/engine/Engine.hpp/.cpp`, `rex-core/src/wallpaper_cli_main.cpp`.

- [ ] Store `HardwareInfo` + `HardwareTier` on the engine at startup (after the renderer device exists), expose
      `const HardwareInfo& GetHardwareInfo() const`.
- [ ] Extend `GET_STATUS` (or `GET_CAPABILITIES`, whichever already carries engine facts — follow its shape) with a
      `hardware` object: `{ activeAdapter, dedicatedVramBytes, av1Main10, hevcMain10, physicalCores, totalRamBytes, tier }`.
- [ ] Verify live: start the runtime, send the command, compare with `--print-hardware` output (must be identical).

**Done when:** the app can read the detected hardware without any new command and without spawning twice.

---

# Phase 2 — Native mode engine

### Task 5: Effective-mode state machine (pure)

**Files:** `rex-core/src/wallpaper/engine/PerfMode.hpp`, `rex-core/tests/wallpaper/test_perf_mode.cpp`,
`rex-core/CMakeLists.txt`.

**Interface:**

```cpp
namespace rex::wallpaper {

enum class PerfSelection { Auto, Power, Balanced, Performance };
enum class PerfMode      { Power, Balanced, Performance };

struct PerfResolution {
    PerfMode  effective;
    bool      pinned;
    bool      onBattery;
    bool      energySaver;
    const char* reasons[2];   // {"on-battery","energy-saver"} subset, in that order; count below
    int       reasonCount;
};

PerfResolution ResolvePerfMode(PerfSelection sel, PerfMode baseline,
                               bool onBattery, bool energySaver) noexcept;

PerfSelection ParsePerfSelection(std::string_view s) noexcept;   // unknown → Auto
const char*  PerfSelectionName(PerfSelection s) noexcept;
const char*  PerfModeName(PerfMode m) noexcept;
}
```

Truth table to encode in the test (24 rows, generated inline):

| selection | onBattery | energySaver | effective | pinned | reasons |
|---|---|---|---|---|---|
| auto | 0 | 0 | baseline | false | — |
| auto | 1 | 0 | power | false | on-battery |
| auto | 0 | 1 | power | false | energy-saver |
| auto | 1 | 1 | power | false | on-battery, energy-saver |
| power | any | any | power | true | — |
| balanced | any | any | balanced | true | — |
| performance | 1 | 1 | performance | true | — |

- [ ] Test first; assert `pinned == (selection != Auto)` for every row.
- [ ] Implement; run to green.

**Done when:** the table passes, including "pinned ignores both triggers" and "both triggers clear only together"
(assert by stepping `1,1 → 1,0 → 0,0` and checking the effective mode stays `power` until the last step).

### Task 6: Presets, overrides, and the cross-language contract

**Files:** `rex-core/src/wallpaper/engine/PerfPreset.hpp`, `rex-core/tests/wallpaper/test_perf_preset.cpp`,
`rex-core/tests/fixtures/perf-presets.json`, `app/electron/perf-presets.cjs`,
`app/electron/perf-presets.test.cjs`, `rex-core/CMakeLists.txt`.

**Interface:**

```cpp
namespace rex::wallpaper {

enum class StreamPreference { Native, Lower };

struct PerfLevers {
    int      frameCapFps        = 0;      // 0 = off
    bool     belowNormalPriority = false;
    bool     pauseOnFullscreen   = true;
    bool     muteAudio           = false;
    bool     forceSdr            = false;
    StreamPreference stream      = StreamPreference::Native;
};

PerfLevers PresetFor(PerfMode mode) noexcept;

// Overrides arrive from the app as an optional-field patch (JSON object, absent = keep preset).
struct LeverPatch {
    std::optional<int>  frameCapFps;
    std::optional<bool> belowNormalPriority, pauseOnFullscreen, muteAudio, forceSdr;
    std::optional<StreamPreference> stream;
};
PerfLevers ApplyPatch(PerfLevers base, const LeverPatch& patch) noexcept;

// Validation: only {0,15,24,30,60} are legal caps; anything else is rejected (returns false) so a
// malformed app payload can never install a 1000 fps cap.
bool ValidateLevers(const PerfLevers&, std::string* error);
}
```

Default table (must equal the spec, §4):

| mode | frameCapFps | belowNormalPriority | pauseOnFullscreen | muteAudio | forceSdr | stream |
|---|---|---|---|---|---|---|
| power | 24 | true | true | false | true | lower |
| balanced | 30 | false | true | false | false | native |
| performance | 0 | false | true | false | false | native |

**Anti-drift contract:** `rex-core/tests/fixtures/perf-presets.json` holds that table. The C++ test asserts
`PresetFor()` equals the fixture; the JS test asserts `DEFAULT_PRESETS` equals the same fixture. One contract, two
implementations, both tested — no silent divergence. The C++ side gets the directory through
`target_compile_definitions(rex-wallpaper-tests PRIVATE REX_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")`
(ctest's working directory is the build tree — a relative path would silently miss, and this fixture must **fail**, not
skip, when absent).

- [ ] Write the fixture.
- [ ] Write the C++ test: fixture equality, patch merge (each field), absent fields preserved, invalid cap rejected,
      `Reset` == `PresetFor` (the reset path is literally re-applying the preset).
- [ ] Write `perf-presets.test.cjs` with `node:test`: fixture equality, `normalizeSelection` (unknown → `auto`,
      case-insensitive, non-string → `auto`), `mergePresets` (partial overrides, per-mode isolation, `{}` → defaults).
- [ ] Implement both sides; `node --test app/electron/perf-presets.test.cjs` and `ctest -R PerfPreset` green.

**Done when:** both suites read the same fixture and pass; adding a lever later requires editing one fixture + two
tests, and a mismatch fails CI.

### Task 7: Live levers (frame cap atomic, priority, pause)

**Files:** `rex-core/src/wallpaper/engine/Engine.hpp/.cpp`.

**Interface:**

```cpp
// Engine.hpp — next to m_loop / m_speed (Engine.hpp:133-134)
std::atomic<int>  m_frameCapFps{0};        // 0 = uncapped; written by the mode apply, read by the render loop
std::atomic<bool> m_perfApplied{false};

void  ApplyLiveLevers(const PerfLevers& next);   // diff against m_appliedLevers, no-op when equal
int   EffectiveFrameCap() const noexcept;        // min(nonzero(userCap), nonzero(modeCap))
```

- [ ] Replace the three unlocked `m_config.maxFps` reads in the render loop (`Engine.cpp:2487,2506,2344`) with
      `m_frameCapFps.load(std::memory_order_relaxed)`, seeded from `m_config.maxFps` in `ApplyPlaybackSettings`
      (`Engine.cpp:1171`) so behaviour is unchanged when no mode is configured.
- [ ] Unit-test the pure composition: `0,0 → 0`; `0,24 → 24`; `60,24 → 24`; `60,0 → 60`; `24,60 → 24`
      (`EffectiveFrameCap` takes both values so it is testable without an engine instance).
- [ ] Priority: `SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS | …)`. Never touch another process.
      Log the change once per transition, not per poll.
- [ ] Pause-on-fullscreen: wire the preset's flag as an **additional** gate on the existing predicate
      (`Engine::CheckFullScreenPause`, `Engine.cpp:1773`); the predicate itself is not modified.
- [ ] No-op guard: `ApplyLiveLevers` compares every field; an identical call returns without logging or touching Win32.

**Done when:** switching 30 ↔ 24 changes the presented cadence within one poll interval, CPU priority changes once, and
a 60 s idle poll with no trigger change produces zero log lines from the apply path.

### Task 8: `SET_MODE` / `GET_MODE` / `INIT`

**Files:** `rex-core/src/wallpaper_cli_main.cpp`, `rex-core/src/wallpaper/engine/Engine.cpp`.

**Payloads:**

```jsonc
// SET_MODE  (app owns selection + presets)
{ "selection": "auto|power|balanced|performance",
  "presets": { "power": {...levers}, "balanced": {...}, "performance": {...} } }

// GET_MODE (same shape for both)
{ "selection": "auto", "effective": "power", "pinned": false, "tier": "strong",
  "hardware": { ... },
  "levers": { ...effective levers actually applied... },
  "triggers": { "onBattery": true, "energySaver": false },
  "reasons": ["on-battery"],
  "pending": ["hdr","stream"] }
```

```cpp
// INIT adds: perfSelection (string) and perfPresets (object, same shape as SET_MODE.presets)
```

- [ ] Parse + validate: bad `selection` → reject with a clear error (do **not** silently keep a half-applied state);
      bad preset → reject the whole command (atomic apply).
- [ ] `GET_MODE` before `SET_MODE` answers from the session defaults (`auto` + built-in presets) — never errors.
- [ ] `pending[]`: compare the effective load-time levers (`forceSdr`, `stream`) with the values the session was
      `INIT`ialised with; report `"hdr"` / `"stream"` when they differ.
- [ ] `SET_MODE` applies live levers immediately, never re-`INIT`s, never reloads the wallpaper.
- [ ] Protocol test: extend the existing `rex-core/tests/wallpaper/test_protocol_safety.cpp` with malformed
      `SET_MODE` payloads (missing selection, unknown selection, wrong types, unknown extra fields) — unknown
      extras ignored, the rest rejected cleanly, and `GET_MODE` still answers afterwards.

**Done when:** `SET_MODE auto → performance → auto` round-trips with the response reflecting the effective mode, and the
wallpaper never flickers.

---

# Phase 3 — App

### Task 9: Persisted state + pure helpers

**Files:** `app/electron/app-state-manager.cjs`, `app/electron/perf-presets.cjs`,
`app/electron/perf-presets.test.cjs`.

```js
performance: {
  selection: 'auto',
  overrides: { power: {}, balanced: {}, performance: {} },
  tier: null,
  detectedAt: null
}
```

- [ ] Add `performance` to the defaults **and** to `mergeWithDefaults`' explicit key list
      (`app-state-manager.cjs:152-170`) — a partially written object must keep sibling defaults.
- [ ] Test: `mergeWithDefaults({ performance: { selection: 'performance' } })` yields the full object with
      `overrides` intact; `mergeWithDefaults({})` yields the default; an unknown selection normalises to `auto`.
- [ ] `load()`/`save()` round-trip through the real file in a temp dir (no Electron), plus a corrupt-file case → defaults.

**Done when:** `node --test app/electron/perf-presets.test.cjs` covers both modules and `app-state.json` written by an
older version loads without losing keys.

### Task 10: IPC + `INIT` plumbing on every spawn path

**Files:** `app/electron/main.cjs`, `app/electron/wallpaperRuntime.cjs`,
`app/electron/supervisor/runtime-manager.cjs`, `app/electron/supervisor.cjs`.

- [ ] `perf:get` → if the runtime is alive (`runtime.proc && runtime.inited`) `send('GET_MODE')`, else answer
      `{ source: 'cache', selection, overrides, tier, detectedAt }`. **Must not spawn** — `send()` spawns on demand
      (`wallpaperRuntime.cjs:172`); guard with an alive check, do not call `send` blindly.
- [ ] `perf:set` → persist first, then `send('SET_MODE', { selection, presets })` if alive; store the last response in
      a `perfState` cache for the tray + the page.
- [ ] `INIT` plumbing: add `perfSelection` + `perfPresets` to **every** call site — interactive apply (`main.cjs:247`),
      boot/reapply (`:140`), crash/`reclaim` recovery (`:567`), and the supervisor's recovery re-apply
      (`supervisor.cjs:275`). Thread through `runtime-manager.init()` (`runtime-manager.cjs:51`).
- [ ] `wallpaperRuntime.init()` currently no-ops once inited (`:191`); keep that (the mode reaches a live runtime via
      `SET_MODE`) — but the **params must still be stored** so a later respawn uses them.
- [ ] Verify: kill the runtime mid-session → supervisor respawns → `GET_MODE` reports the same effective mode (this is
      review focus #5).

**Done when:** a forced crash loop keeps the mode, `perf:get` with no runtime leaves `proc == null`, and no spawn is
triggered by reading state.

### Task 11: Performance page

**Files:** `app/src/settings/PerformancePage.jsx`, `app/src/settings/performance.css`,
`app/src/settings/levers.js`, `app/src/App.jsx`, `app/src/components/SidebarLayout.jsx`.

Layout (top → bottom):

1. **Selection** — four radio cards: Auto / Power saving / Balanced / Performance. Selecting one calls `perf:set`
   and pins immediately (no Apply button).
2. **Status line** — `Auto → Power saving — on battery` · `Pinned: Performance — battery and Energy Saver ignored`.
3. **Hardware** — GPU name, dedicated VRAM (GB), `AV1 Main10`/`HEVC Main10` chips (supported / not), tier badge,
   `detectedAt`. Tier comes from the native response; when the runtime is down, show the cached value with a subtle
   "as of <date>" note.
4. **Levers, per mode** — each mode is a collapsible group with its levers. Editing any lever saves to
   `performance.overrides[mode]` and re-sends `SET_MODE`. Live levers show "applies now"; `forceSdr` and `stream` show
   a **Pending until the next wallpaper load** badge when they differ from the applied session value.
5. **Reset to preset** per mode.

- [ ] `levers.js` holds only labels/options/allowed values (no business logic, no defaults — those come from
      `perf-presets.cjs`), so a lever added once appears in every mode editor.
- [ ] The pinned badge is driven by `pinned` from the native response, **not** recomputed in the UI.
- [ ] Route: `App.jsx` renders `PerformancePage` for `tab === 'Performance'`; `SidebarLayout.jsx` re-adds
      `{ icon: Gauge, label: 'Performance' }` to the "Desktop" group after Wallpapers.
- [ ] Empty/error states: runtime down → controls still work (they persist; apply on the next spawn); native error →
      inline message, previous value kept (never leave a half-applied UI).
- [ ] `pnpm lint` + `pnpm typecheck` clean; live pass through every control.

**Done when:** every acceptance line in spec §9.3 is visible on screen and the pinned line appears for a pinned mode on
battery.

### Task 12: Tray

**Files:** `app/electron/tray-manager.cjs`, `app/electron/main.cjs`.

- [ ] Add a `Performance` submenu with four `type: 'radio'` items bound to the cached selection; clicking one calls the
      same `perf:set` path as the page.
- [ ] Keep the submenu in sync when the page changes the selection (main pushes the new state to the tray).
- [ ] Sensor-less environments: the submenu still renders (selection from cache).

**Done when:** tray and page never disagree, and a tray change updates the page without a reload.

---

# Phase 4 — Load-time levers (next load only)

### Task 13: Force SDR as an `INIT` parameter

**Files:** `rex-core/src/wallpaper_cli_main.cpp`, `rex-core/src/wallpaper/engine/Engine.cpp`,
`app/electron/main.cjs`, `app/electron/wallpaperRuntime.cjs`.

- [ ] `forceSdr` is CLI-only today (`wallpaper_cli_main.cpp:809`); add it to `INIT` (boolean, default false) and use it
      where the CLI flag is consumed, so an `INIT` spawn behaves identically to `--force-sdr`.
- [ ] The app sends the effective mode's `forceSdr` in `INIT` on every spawn.
- [ ] Verify on an HDR monitor (or the closest available): `INIT { forceSdr: true }` produces the SDR path; a `SET_MODE`
      with `forceSdr` differing only shows `pending:["hdr"]` and does **not** change the running output.
- [ ] If no HDR display is available, record the test as blocked-for-hardware rather than claiming it (see
      `TODO.md` P2-4).

### Task 14: Lower video-stream preference

**Files:** `rex-core/src/wallpaper/decoder/FfmpegVideoDecoder.cpp` (`SelectStreamByResolution`,
`:570-604`), `rex-core/src/wallpaper/engine/VideoManager.cpp`, `rex-core/src/wallpaper_cli_main.cpp`.

- [ ] Pass the preference into stream selection: `Lower` = prefer a stream whose width **and** height are ≤ 50 % of the
      monitor's; otherwise keep the current pick (the largest/default stream). Never *upscale*, never pick a smaller
      stream than required by the monitor.
- [ ] Multi-stream fixture: a file with 4K + 1080p → `Lower` selects 1080p, `Native` selects 4K (assert the selected
      stream index in a unit test around `SelectStreamByResolution`).
- [ ] Single-stream fixture: `Lower` selects the same stream and logs "no lower stream available" **once** (the UI
      shows the lever as having no effect for this file).
- [ ] Live: switching `Lower` on the running wallpaper shows `pending:["stream"]`; the next load uses it (log the chosen
      stream index and its resolution).

**Done when:** the same file at `Native` and `Lower` reports different stream indices when a lower stream exists, and
identical indices (plus one log line) when it does not.

---

# Phase 5 — Verification and tracking

### Task 15: Full matrix + docs

- [ ] **Automated:** full `ctest` (must exceed the Task 0.1 baseline count by the new suites), `node --test`,
      `pnpm lint`, `pnpm typecheck`.
- [ ] **Live matrix** (record in the plan's task log and in `TODO.md`):

| Case | Expected |
|---|---|
| Auto, AC, desktop | baseline mode for the tier, `reasons: []` |
| Auto, battery | Power saving, `reasons: ["on-battery"]` |
| Auto, battery + Energy Saver → unplug Energy Saver | still Power saving, then baseline when battery clears |
| Pinned Performance, battery + Energy Saver | Performance, `pinned: true`, UI pinned line |
| Switch 30 ↔ 24 ↔ off mid-playback | no black frame, no re-init, cadence changes |
| Edit `forceSdr` / `stream` mid-playback | `pending` badge, no output change |
| Next wallpaper apply | pending levers applied, `pending: []` |
| Kill runtime | respawn keeps the effective mode |
| Lock → "stop (free VRAM)" | dedicated VRAM drops; wallpaper returns on unlock |
| Single-stream file + `Lower` | no decode change, UI says so |

- [ ] **Trackers:** per `AGENTS.md` — check off the new item(s) in `TODO.md` and add the shipped result to
      `everything.md`; update `#project_status.md` (feature) and the `README.md` status line if the standing changes.
- [ ] **Docs:** update the spec if any decision changed during implementation (the spec is canonical; the plan may not
      silently diverge).

**Done when:** every row above has an observation, the trackers are updated, and no row is marked done on inference.

---

## Sequencing summary

```
0.1 baseline → 0.2 P3 re-pin → 0.3 P3 VEH → 1 measurement
  → 2 tier (pure) → 3 probe → 4 protocol
  → 5 mode (pure) → 6 presets + fixture → 7 live levers → 8 protocol
  → 9 app state → 10 IPC/INIT → 11 page → 12 tray
  → 13 force SDR → 14 stream
  → 15 verification + trackers
```

Tasks 2/3/4 and 5/6 are independently landable; 7 requires 5+6+8's shapes; 11 requires 9+10. Do not start Phase 3
before Phase 2's protocol is live — the UI would have nothing honest to display.
