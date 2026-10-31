# Paiint Test Mode Diagnostics + Window-Aware Wallpaper Audio — Design

**Status:** approved 2026-10-29 · **Scope:** Electron app (`app/`), no native change.
**Product:** Paiint by MoiNeX · **Distribution:** Microsoft Store (MSIX), closed source.

---

## 1. Why

Two gaps block the next round of engine work:

1. **We cannot see what the engine saw.** The HDR red-only investigation needed
   adapter LUIDs, the exact display mode, driver version, HDR state, nits,
   decode/present timings and the runtime log — gathered by hand from three
   terminals and two ad-hoc scripts. A user reporting "it looks wrong" gives us
   none of that.
2. **Wallpaper audio competes with the app.** A video wallpaper with sound keeps
   playing while the user is configuring Paiint. There is no way to say "only
   let the wallpaper speak when I am not looking at Paiint".

The first is solved by a **local-only test-mode recorder** that writes a
crash-safe, auto-updating record to Downloads. The second by a **Settings
toggle** that mutes the wallpaper while a Paiint window is open and restores
the wallpaper's own mute choice when no window is open.

Nothing in this design leaves the PC. There is no network code, no upload, no
account, no analytics endpoint.

---

## 2. Decisions

| # | Decision | Why |
|---|---|---|
| D1 | Diagnostics is a **mode in the one app**, not a forked build | One codebase, one MSIX, one runtime. Settings → Advanced toggle plus `Paiint.exe --diagnostics`. |
| D2 | **Host-only recorder** in the Electron main process; no new native `GET_DEVICE_INFO` command | The host can already get `app.getGPUInfo('complete')`, `screen`, `powerMonitor`, `os`, WMI and the engine's `GET_STATUS`. A native command would force an engine rebuild and a second protocol surface for no extra data. |
| D3 | Output goes to **`Downloads\Paiint Diagnostics`** | The user asked for Downloads, and it is the one folder a non-technical tester can find. If Downloads is not writable, fall back to `<userData>\diagnostics` and record the fallback in the status. |
| D4 | **Full monitor identity minus serial** | Manufacturer, model, product code, year/week, diagonal size, connection type, EDID identity. Serial numbers are removed before anything is written. |
| D5 | Sampling: **4 Hz for 15 s after each apply, then 1 Hz**; `GET_STATUS` with a **1.5 s timeout**, skipped on timeout | The first seconds after a scene load are where decode/present pathologies live; steady state only needs a heartbeat. A wedged engine must never block the recorder. |
| D6 | **200 MB per session cap**, keep the **last 10 sessions** | A tray app can run for days. The cap is a hard stop, not a rotation that loses the beginning. |
| D7 | Audio key is **`engine.audioOnlyWhenWindowClosed`**, default **false** | Off preserves today's behaviour. The name says exactly what it does. |
| D8 | The override can only **add mute**; it never unmutes a wallpaper the user muted | The per-wallpaper `playback.mute` remains the user's intent. The override is a temporary, visibility-driven mute on top. |
| D9 | `lastApplied.params.muted` stores **intent**, never the temporary effective value | Crash replay and the next apply must restore what the user chose, not what the window state happened to be at sample time. |
| D10 | No privacy-policy data-collection declaration change; one local-recording disclosure line | The recorder never transmits. The Store declaration stays "no data collected". |

---

## 3. Window-aware wallpaper audio

### 3.1 What counts as "the desktop is covered"

The desktop is **covered** when any visible top-level window meaningfully covers
a monitor. A background PowerShell process (`occlusionMonitorScript()` in
`main.cjs`) enumerates windows in Z-order every 500 ms and prints one line per
change (`CLEAR` or `OCCLUDED <class> <w>x<h>`); `app/electron/occlusion.cjs`
parses that wire format.

A window counts when it is visible, not minimized, not DWM-cloaked, not the
taskbar, and covers at least **0.5 % of a monitor's area** (minimum 4096 px²).
The threshold keeps a tiny always-on-top overlay (a 120x16 password-manager bar,
a clock widget) from silencing the wallpaper forever.

The desktop window itself (`GetShellWindow()` / `Progman` / `WorkerW`) is the
bottom of the normal Z-order stack: reaching it ends the scan as **clear**. That
is what makes Win+D — which raises the desktop above every window — report clear,
while clicking the desktop with windows still visible stays occluded.

Until the monitor has reported once, the decision falls back to "a Paiint window
is open" (mute, the safe default). After that the enumeration is authoritative:
it already includes Paiint's own window when it covers the desktop, and it knows
when the desktop is genuinely visible again even though `isWindowOpen()` still
returns true (Win+D never minimizes the window).

### 3.2 Mute truth table

| User intent `muted` | `audioOnlyWhenWindowClosed` | Desktop covered | Effective mute sent to engine |
|---|---|---|---|
| false | false | false | false |
| false | false | true | false |
| false | true | false | false |
| false | true | true | **true** |
| true | either | either | **true** |

The pure module `app/electron/audio-visibility.cjs` owns this table:
`isWindowOpen(win)`, `desktopCovered({ occlusionReported, desktopOccluded,
windowOpen })` and `effectiveMute({ intentMuted, onlyWhenClosed, windowOpen })`.

### 3.3 Where it is applied

| Moment | Behaviour |
|---|---|
| Video apply (`applyVideoEntry`, including playlist and recovery) | `LOAD` receives the **effective** mute; `lastApplied.params.muted` stores the **intent**; the occlusion monitor starts. |
| Occlusion monitor reports a change | `SET_MUTE` receives the **effective** mute; an `occlusion` event and an `audio` event are recorded. |
| Live `wallpaper:update` with `muted` | `SET_MUTE` receives the **effective** mute; the persisted intent is the value the UI sent. |
| Window `show` / `restore` | Recompute; a video scene is muted if the desktop is covered. |
| Window `hide` / `minimize` / `closed` | Recompute; a video scene is unmuted when the setting is on, the desktop is clear and the intent is not muted. |
| Setting toggled | Start/stop the monitor and recompute immediately. |
| Image apply / playlist Stop | Stop the monitor (no video, no audio); reset to the safe fallback. |
| No video scene / runtime stopped | No-op; never spawn a runtime to change mute. |

The window events are wired in `createWindow()` and the recompute helper is
called from the app-state setter. The helper never changes `lastApplied`.

---

## 4. Test-mode diagnostics

### 4.1 Activation

- `diagnostics.enabled` in `app-state.json`, default **false**.
- `Paiint.exe --diagnostics` forces the recorder on for that session. It does
  not write `diagnostics.enabled`.
- Settings → Advanced → **Test mode diagnostics** writes the state key; main
  starts or stops the recorder from the `app-state:set` handler.
- The status row shows whether the recorder is running and the real folder.
  A `--diagnostics` session reports `forced: true`.

### 4.2 Folder layout

```
%USERPROFILE%\Downloads\Paiint Diagnostics\
  device.json                  static device identity, rewritten on each start
  current.jsonl                live append-only record (one JSON object per line)
  current-summary.json         rewritten every 5 s; safe to read at any moment
  sessions\
    2026-10-29T14-05-33-123Z\
      session.jsonl            the session's current.jsonl, moved on archive
      summary.json             final summary
      device.json
      runtime.log
      app.log
      crash.log
      crashes\*.dmp
```

Fallback root when Downloads is not writable:
`<userData>\diagnostics\` with the same layout; `diagnostics:status` reports
`fallback: true` and the real path.

### 4.3 Record format

`current.jsonl` starts with a session line, then samples and events:

```json
{"t":"2026-10-29T14:05:33.123Z","seq":1,"kind":"session","session":"2026-10-29T14-05-33-123Z","appVersion":"0.1.0","forced":false}
{"t":"...","seq":2,"kind":"sample","app":{...},"window":{...},"displays":[...],"power":{...},"engine":{"available":true,"data":{...}},"lastApplied":{...},"playlist":{...},"audio":{...}}
{"t":"...","seq":3,"kind":"event","event":"apply","data":{"source":"wallpaper:video","kind":"video","path":"..."}}
```

Every line is written with a single append; a crash can lose at most the
current line, never the file. `seq` is monotonic within a session.

`current-summary.json`:

```json
{
  "generatedAt": "...",
  "session": { "startedAt": "...", "dir": "...", "appVersion": "0.1.0", "forced": false },
  "counts": { "samples": 42, "events": 7, "bytes": 123456 },
  "latestSample": { ... },
  "engine": { "available": true, "data": { ... } },
  "lastEvents": [ ... last 20 events ... ],
  "capReached": false
}
```

It is written atomically (`.tmp` + rename) so a reader never sees half a file.

### 4.4 What a sample contains

| Group | Fields |
|---|---|
| App | uptime, `process.memoryUsage()`, `app.getAppMetrics()` (pid, type, cpu, working set, private bytes) |
| Window | exists, open, visible, minimized, focused, bounds |
| Displays | id, bounds, work area, scale factor, refresh, colour space, rotation |
| Power | on battery, idle time (`powerMonitor.getSystemIdleTime()`, seconds), thermal state when available |
| Engine | `{available, data}` from `GET_STATUS` (adapter LUIDs, same-adapter flags, HDR active, nits, Hz, decode/present percentiles, queue/drop counters — whatever the runtime reports) |
| Last applied | kind, path, mode, loop, muted intent, volume, speed, audio mode, brightness/contrast/saturation |
| Playlist | mode, queue length, current index/path, next rotation time |
| Audio | setting, intent, Paiint window open, desktop occluded, monitor-reported, occlusion detail, effective mute |

Every group is collected through `sampleField()`, so one failing Electron API
can only blank its own group — it must never erase the whole sample. (Real
tester sessions contained nothing but
`{"error":"powerMonitor.getIdleTime is not a function"}`.)

Events recorded immediately (not sampled): apply start/result, runtime
event stream **except `FRAME_READY`** (a ~30 Hz presentation heartbeat that
would fill the 200 MB cap in hours), supervisor events, window state changes,
`audio` decisions (only when the decision changes), `occlusion` state changes,
settings changes, recorder start/stop/archive/cap.

### 4.5 Device identity (`device.json`)

- **App/build:** name, version, packaged, locale, Electron/Chrome/Node/V8
  versions, architecture.
- **OS:** platform, release, version, architecture, total memory, CPU model,
  speed, core count.
- **Displays:** Electron `Display` fields above, plus primary display id.
- **GPU:** sanitized `app.getGPUInfo('complete')` `gpuDevice[]` entries
  (vendor id/string, device id/string, driver version/date, active) and
  `app.getGPUFeatureStatus()`.
- **Windows hardware (PowerShell/WMI):**
  - `WmiMonitorID`: instance name, manufacturer, model, product code,
    year/week. **`SerialNumberID` is never read into the output.**
  - `WmiMonitorBasicDisplayParams`: max width/height in cm → diagonal inches.
  - `WmiMonitorConnectionParams`: numeric video output technology.
  - `Win32_VideoController`: name, driver version/date, adapter RAM, current
    width/height/refresh.
  - Registry: HAGS (`HwSchMode`), MPO (`OverlayTestMode`), Game Mode
    (`AutoGameModeEnabled`).
- If PowerShell/WMI fails, `hardware: { error: "..." }` is recorded; the rest
  of `device.json` still exists.

### 4.6 Lifecycle

| Event | Action |
|---|---|
| Recorder start | Create root + session dir; write session line; write `device.json`; start timers (unref'd); take an immediate sample. |
| Apply | `markApply()` sets the 15 s fast window and takes an immediate sample. |
| Every 5 s | Rewrite `current-summary.json`. |
| Disable / quit | Stop timers; final summary; move `current.jsonl` to `session.jsonl`; copy summary/device/logs/crashes into the session dir; prune to 10 sessions. |
| Crash | `current.jsonl` and the last summary remain; the next start reads the session record from the head of `current.jsonl`, reuses that session directory (moves the file in as `session.jsonl`) and prunes any stale empty session dirs. A file without a readable session record falls back to `recovered-<start-time>`. |
| 200 MB reached | Append one `cap-reached` event, stop sampling and events, keep the files. |

On quit, archiving happens **after** the native runtime has drained (so
`runtime.log` is complete) and **before** `app.quit()` finishes.

### 4.7 Privacy and security

- No network code, no upload, no external URL.
- No serial numbers, no Windows product key, no user name, no email.
- Media file paths are included because they identify which file misbehaved;
  they stay local.
- The folder is ordinary user data under Downloads; the user can delete it at
  any time.
- `docs/legal/PRIVACY.md` gets one disclosure row and one sentence; the Store
  data-collection declaration remains "no data collected".

---

## 5. Interfaces

### 5.1 Pure modules

`app/electron/audio-visibility.cjs`

```js
isWindowOpen(win) -> boolean
effectiveMute({ intentMuted, onlyWhenClosed, windowOpen }) -> boolean
```

`app/electron/diagnostics/device-info.cjs`

```js
decodeWmiString(value) -> string
monitorSizeInches(maxHcm, maxVcm) -> number | null
sanitizeMonitor(monitor) -> object            // removes every serial field
pickGpuDevices(gpuInfo) -> object[]
parseHardwareJson(text) -> { monitors, gpus, registry }
collectDeviceInfo({ app, screen, execFile, platform, osModule }) -> Promise<object>
```

`app/electron/diagnostics/recorder.cjs`

```js
new DiagnosticsRecorder({ rootDir, now, fsModule, pathModule, log,
  getStatus, collectSample, maxBytes, keepSessions, fastMs, fastWindowMs,
  slowMs, summaryMs })
start()
setDeviceInfo(info)
event(name, data)
markApply(reason)
sampleNow()
writeSummary()
status()
stop()
archive(reason)
```

### 5.2 Main process

New IPC:

| Channel | Returns |
|---|---|
| `diagnostics:status` | `{ enabled, forced, dir, sessionDir, bytes, samples, events, lastSampleAt, capReached, fallback }` |
| `diagnostics:open-folder` | `{ ok, dir }`; creates the folder if needed |

`app-state:set` reacts to:
- `diagnostics.enabled` → start/stop the recorder;
- `engine.audioOnlyWhenWindowClosed` → recompute effective mute.

Preload exposes:

```js
diagnostics: {
  status: () => ipcRenderer.invoke('diagnostics:status'),
  openFolder: () => ipcRenderer.invoke('diagnostics:open-folder'),
}
```

### 5.3 Settings UI

Engine section, below the existing engine toggles:

- **Play audio only when the desktop is clear** (`engine.audioOnlyWhenWindowClosed`).
  Description: mutes the wallpaper while any window covers the desktop; audio
  returns when the desktop is visible again (all windows minimized, or the
  desktop shown with Win+D).

Advanced section (new, collapsed by default):

- **Test mode diagnostics** (`diagnostics.enabled`).
  Description: records engine/display/device details to
  `Downloads\Paiint Diagnostics`; local only.
- Status row: **Diagnostics folder** with the real path and an **Open** button.

---

## 6. Failure modes the design must prevent

1. **Recorder blocks the engine.** A `GET_STATUS` that hangs must time out at
   1.5 s and be skipped; the sample still records the rest.
2. **Recorder spawns a runtime.** It checks `runtime.proc` first; a stopped
   engine is recorded as `{available:false, reason:'runtime-stopped'}`.
3. **Crash loses the record.** Every line is appended; summary is atomic;
   archive only happens on a clean stop.
4. **Unbounded disk use.** 200 MB hard cap; 10 sessions retained.
5. **Audio override becomes the new intent.** `lastApplied.params.muted` is
   never overwritten by the visibility override.
6. **Override unmutes a muted wallpaper.** The table only adds mute.
7. **A closed window leaves the wallpaper muted forever.** `hide`, `minimize`
   and `closed` all recompute.
8. **Settings and tray disagree.** Both read the same app-state key; the
   app-state setter is the single trigger.
9. **Device identity leaks a serial.** `sanitizeMonitor` strips every key whose
   name contains `serial`, and the PowerShell projection never selects
   `SerialNumberID`.
10. **Downloads is unwritable.** Fall back to `<userData>\diagnostics`, report
    `fallback: true`, and keep recording.

---

## 7. Acceptance criteria

- Turning on Test mode diagnostics creates
  `Downloads\Paiint Diagnostics\current.jsonl` within 2 s and the file grows
  while Paiint runs; `current-summary.json` is valid JSON and refreshes every
  ~5 s.
- Applying a video produces 4 Hz samples for ~15 s, then ~1 Hz.
- Killing the runtime mid-session leaves a parseable `current.jsonl`; the
  next start writes a new session.
- Turning the toggle off archives the session under `sessions\` and removes
  the live files.
- Quitting archives `session.jsonl`, `summary.json`, `device.json`,
  `runtime.log`, `app.log`, `crash.log` and any `crashes\*.dmp`; the 11th
  session prunes the oldest.
- `device.json` contains monitor manufacturer/model/product code/year/week and
  no serial number anywhere.
- With the audio toggle on and a video playing: a visible window covering the
  desktop mutes (even when it is not focused); minimizing everything or showing
  the desktop with Win+D unmutes (when the wallpaper's own mute is off);
  restoring/raising a window mutes again; a window covering less than 0.5 % of a
  monitor does not mute; turning the toggle off restores the wallpaper's own
  mute.
- Every mute decision change writes an `audio` event, and every occlusion change
  writes an `occlusion` event, so a tester session can be audited without the
  GUI.
- A playlist rotation and a crash-recovery replay respect the same audio rule.
- `node --test` on the three new test files, `pnpm --dir app lint`,
  `pnpm --dir app build`, `pnpm --dir app check:palette` and root
  `pnpm typecheck` all pass.

---

## 8. Out of scope

- Native `GET_DEVICE_INFO` / `QueryDisplayConfig` / advanced-colour APIs.
- Uploading diagnostics, a support bundle UI, or a network endpoint.
- A diagnostics viewer inside Paiint (the file and Explorer are enough).
- Per-monitor audio routing, volume ducking, or audio device selection.
- Widgets, taskbar, themes, marketplace, automation and SDK work.
