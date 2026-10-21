# REX — Settings research (2026-10-21)

**Question:** what should REX's *settings* contain — the ones a user reaches for
to make the wallpaper engine behave on their machine — and which of those can
REX actually honour today?

**Verdict labels:** LIVE = the runtime can apply it now, no engine change ·
FOUNDATION = the engine implements it, but the app cannot reach it ·
INIT-ONLY = the engine honours it, but only at process start (needs a runtime
restart) · GAP = missing entirely · DEAD = a setting that exists in the UI or
the data model and does nothing.

Method: a full code audit of the runtime command surface
(`rex-core/src/wallpaper_cli_main.cpp`), the engine's live property paths
(`rex-core/src/wallpaper/engine/Engine.cpp`, `runtime/RendererAdapters.cpp`),
the app's IPC + persistence layers (`app/electron/*`, `app/src/wallpapers/*`),
and a teardown of the two reference products (Wallpaper Engine's settings +
application rules, Lively Wallpaper's Performance and Video pages) plus Windows
Personalization. No public per-control analytics exist for any of these apps;
§7 proposes a local measurement plan.

Companion: [`research-inspector.md`](research-inspector.md) (what belongs in
the *per-wallpaper* panel). This document is about the **global** surface.

---

## 1. The first finding: there is no settings surface to add things to

The app has a **Settings** nav item, a **Settings** tray item, and a `PAGES`
table that describes a Settings page with the subtitle *"How REX behaves on
your machine."* — and none of it renders:

| Piece | Where | Reality |
|---|---|---|
| `PAGES` table (3 pages, with subtitles) | `app/src/App.jsx:21-31` | **Never read.** Nothing in the app references `PAGES`; the page head renders the raw tab name (`App.jsx:104`). Even the Wallpapers subtitle is dead. |
| Settings nav item | `app/src/components/SidebarLayout.jsx:28-34` | Selects the tab → renders an `<h2>Settings</h2>` and **no body**. |
| Settings tray item | `app/electron/tray-manager.cjs:99-102` | `showWindow('Settings')` → the same empty page. |
| Page body | `app/src/App.jsx:107` | `{tab === 'Wallpapers' ? … : null}` — **only the Wallpapers tab has content.** |

So the "Settings" the user can click today is a **stub with a heading**. That is
the whole answer to "what to add in settings": first there has to be a page.
Everything below is what should go on it.

There is a second, smaller truth in the same place: the two dead `Property*`
surfaces below (§4) mean the *data model* is much richer than the *UI*. The
mistake to avoid is filling a new Settings page from that model — that would
re-create the exact disease the Inspector cleanup just cured (controls that
look real and change nothing).

---

## 2. What the engine can actually do (the hard constraint)

The runtime's command surface is `wallpaper_cli_main.cpp:833` onward. The
property setters are one block, `:1122-1180`:

| Command | Property | Live? | Engine path |
|---|---|---|---|
| `SET_FILL_MODE` | `fillMode` | **live** | `Engine.cpp:1065` → per-frame `ComputeFillViewport` |
| `SET_VOLUME` | `volume` | **live** | `Engine.cpp:1137` → `AudioPlayer` |
| `SET_SPEED` | `speed` | **live** | `Engine.cpp:1169` → `VideoPlaybackController` |
| `SET_LOOP` | `loop` | **live** | `Engine.cpp:1158` → decoder |
| `SET_MUTE` | `mute` | **live** | `Engine.cpp:1147` |
| `SET_BRIGHTNESS` | `brightness` | **live** | `Engine.cpp:1083` → `D3D12Renderer` b0 constant, re-packed **every frame** (`D3D12Renderer.cpp:2590`) |
| `SET_CONTRAST` | `contrast` | **live** | `Engine.cpp:1101` → same constant |
| `SET_SATURATION` | `saturation` | **live** | `Engine.cpp:1119` → same constant |
| `SET_AUDIO_MODE` | `audioMode` | **live** | `Engine.cpp:1230` → endpoint re-negotiation on the audio thread |
| `SET_MONITOR` | — | no-op for placement | `Engine.cpp:953` — updates the current monitor + cached Hz, **does not move the wallpaper** |
| `PAUSE` / `PLAY` / `STOP` / `UNLOAD` | — | **live** | `Engine.cpp:803/848/876/735` |
| `GET_STATUS` | — | read | `:1325-1427` — ~60 telemetry fields |
| `GET_CAPABILITIES` | — | read | `:1429-1441` — the adapter's property list |

**Nine properties are live. The app forwards six.** `main.cjs:239` maps only
`SET_FILL_MODE / SET_VOLUME / SET_SPEED / SET_LOOP / SET_MUTE / SET_AUDIO_MODE`
— `SET_BRIGHTNESS`, `SET_CONTRAST` and `SET_SATURATION` are missing from the
map, so `sendIfSupported` skips them. The runtime is *already advertising*
them: the video adapter's property list contains all three
(`RendererAdapters.cpp:82-84`), and `capabilities.js:11-13` intersects that
live list with the app's static fallback map — which is what drops them
(`desktop.js:31-38`). The engine has a complete D3D12 colour-adjustment
pipeline for this (`D3D12Renderer.cpp:2589-2593` packs
brightness/contrast/saturation/paper-white into the b0 root constants every
frame) and the app never sends a byte of it.

Two more engine paths exist and are unreachable over the wire:

- **rotation / flipH / flipV** — implemented in `Engine::SetProperty`
  (`:1277-1290`) and advertised by *both* adapters
  (`RendererAdapters.cpp:58-60, 77-79`), but the dispatcher has **no
  `SET_ROTATION` / `SET_FLIP` branch**, so the property can never be set from
  outside the process. The static (image) adapter even advertises them.
- **audio-reactive + sensitivity** — `Engine::EnableAudioReactive` /
  `SetAudioSensitivity` (`:1310, 1317`) mutate `m_config`, but no command
  reaches them.

### INIT-time knobs (the "global engine behaviour" surface)

`INIT` accepts three behavioural knobs (`wallpaper_cli_main.cpp:903-910`) and
`WallpaperConfig` defines them (`rex-core/include/rex/wallpaper/IWallpaperEngine.hpp:103-119`):

| Knob | Default | App sends today | Effect |
|---|---|---|---|
| `maxFps` | 60 | `fps: 0` from `desktop.js:61` → clamped 0…240 | Presentation budget. `userCap <= 0` = uncapped, so a video today presents at **display refresh**, not 60 (`CadenceController.hpp:21-35`). |
| `hardwareAcceleration` | true | **always true** (`main.cjs:589`) | HW decode on/off. Lively exposes exactly this toggle for unstable GPUs. |
| `pauseOnFullscreen` | **true** | **never sent** | The engine auto-pauses when a true full-screen window owns the foreground (`Engine.cpp:1824-1874`, monitor-covering test, shell-chrome excluded). REX therefore pauses on fullscreen apps **with no user control over it.** |

`forceSdr` is not a protocol command at all — it is a **spawn flag**
(`--force-sdr`, `wallpaper_cli_main.cpp:2505`), i.e. it can only change when the
runtime process starts.

### Policy the engine hard-codes (no channel to change it)

`Engine::CheckSuspendSources` (`:1925-1957`) pauses the wallpaper when the
**session locks**, when the **display turns off**, or when **power saving** is
on / the battery is ≤15 % off AC (`PowerPolicy.hpp:15-20`). All three are
hard-coded, always-on, and unreachable from the app.

---

## 3. Competitor teardown — what people actually configure

Both reference products put the same four things at the top of their settings,
and REX already owns two of them.

| Setting | Wallpaper Engine | Lively | REX today |
|---|---|---|---|
| **Pause behaviour** | "Other application fullscreen" → Stop (free memory) / pause / mute; **Application Rules** per `.exe` (condition *is running* / *is playing audio* → *stop / keep running / pause / mute*) | **Pause Algorithm**: Grid / All Process / Foreground Process / Direct3D; "pause on battery"; "play wallpaper only on desktop" | Fullscreen pause exists **but is not user-configurable**; no per-app rules at all |
| **Placement / fit** | per-wallpaper properties + global default | scaling default, placement *per / span / duplicate* | ✅ fit modes (Inspector) |
| **Performance** | frame-rate, quality, per-app override | **GPU decode toggle**, pause algorithm, battery | frame cap not settable; HW decode not settable |
| **Startup** | auto-start at boot, screensaver, per-app wallpapers | startup behaviour, screensaver | login item exists in the tray only (`main.cjs:886`) |
| **Multi-monitor** | per-monitor wallpapers + **profiles** | per-monitor | EDID-keyed persistence exists in the engine; no UI (`TODO.md` P3-2) |
| **Diagnostics** | debug log, scan tool | "Report Bug" + coverage overlay | ~60 telemetry fields in `GET_STATUS`, **no app path at all** |

The two gaps that stand out: **pause policy** (both competitors make it a
headline; REX has half the machinery and no lever) and **diagnostics** (REX
measures everything and shows nothing).

---

## 4. Settings that already exist and do nothing (DEAD)

Before adding anything, this is the list of controls that are *already shipped*
and are inert. They are the anti-pattern this document exists to prevent.

### 4.1 Eight dead Inspector sections — ~75 store keys, zero consumers

`app/src/wallpapers/inspector/sections/` contains eight fully-written
components that **nothing imports** (the Inspector mounts five other files,
`Inspector.jsx:12-16`):

| Section | Model group | Keys | Verdict |
|---|---|---|---|
| `GeneralSection` | `general.*` | 9 | DEAD — `general.favorite` is read by the playlist engine, but it **defaults to `true` for every wallpaper** (`registry.js:175`), so "Favorites Only" keeps everything (already tracked as **P3-5c** in `TODO.md`) |
| `PerformanceSection` | `performance.*` | 9 | DEAD — but `frameRateLimit` / `pauseFullscreen` map onto real engine knobs (§2) |
| `MonitorSection` | `monitor.*` | 7 | DEAD — the engine's per-monitor path exists (`LOAD` `monitor` param, EDID persistence) but `SET_MONITOR` cannot move a wallpaper |
| `InteractionSection` | `interaction.*` | 8 | DEAD — no engine support |
| `AudioSection` | `audio.*` | 8 | DEAD — except `audioReactive`, which the engine implements (`Engine.cpp:1310`) and the protocol cannot reach |
| `ScheduleSection` | `schedule.*` | 7 | DEAD — the playlist engine has its own (implemented) scheduling |
| `AdvancedSection` | `advanced.*` | 8 | DEAD — no engine support |
| `InfoSection` | `info.*` | 21 | DEAD — and its "real" fields are registry placeholders (`registry.js:376-388`): every file claims `1920 × 1080` / `60 fps` / `48.2 MB` / `2026-08-01` |

The registry declares **~134 property keys** across 12 groups plus per-type
renderer defaults. **Exactly 6 are reachable** from the UI
(`wallpaperSettings.js:42-49`). The rest are a spec that was never built.

### 4.2 Seven no-op toggles in the playlist dialog

`PlaylistEngineDialog.jsx:409-413` renders the seven `multiMonitor.*` switches
(*per monitor, mirror, independent timers, synchronized, independent
transitions, independent randomization, span*). The engine **never reads
`settings.multiMonitor`** — the only two references in the whole app are the
default (`engine.js:230`) and the toggle itself. They are decoration.

### 4.3 Playlist filters/rules that read placeholder data

`engine.js:385-413` filters on fields that real wallpapers do not carry:

| Control | Reads | Reality for imported files |
|---|---|---|
| Min Rating | `w.general.rating` | **undefined** — the rating lives at `info.rating` (`registry.js:387`), not `general.rating` → the filter never matches anything |
| Max Duration | `w.media?.duration` | **`w.media` does not exist** in the model at all → the filter is always 0 → never filters |
| Only HDR | `w.renderer?.hdr` | **undefined** — no renderer default carries `hdr` → excludes everything |
| Only 4K | `w.info.resolution` | the registry placeholder `"1920 × 1080"` → never matches a real 4K file |
| Skip NSFW | `w.info?.nsfw` | never set anywhere |
| Skip Large Files | `w.info?.fileSize` | set only for `File`-object imports (`store.js:422`), **not** for path-restored entries |
| Favorites Only | `w.general.favorite` | defaults `true` for every file → keeps everything (P3-5c) |

### 4.4 `automation.*` — implemented, but on simulated signals

`engine.js:229` declares ten automation keys and `:766-815` evaluates them, but
their inputs are developer-only: weather, lock, game, docked, presenting,
screen-sharing, remote-desktop and lid-closed are **only** settable from the
dialog's *Developer Options* buttons (`PlaylistEngineDialog.jsx:502-511`), and
CPU/GPU are **random numbers** (`engine.js:837-838`). Exposing automation as a
user setting would ship a control that cannot work outside the dialog.

### 4.5 `theme.*` — contradicts the design decision

`app-state.json` carries `theme: { themeId: 'midnight-glass', colorTokens: {} }`
(`app-state-manager.cjs:35`) and nothing reads it. The colour spec
(`docs/superpowers/specs/2026-10-20-app-color-palette-design.md`) deliberately
defines **one** palette, "Obsidian & Champagne". A theme picker would undo
that; the key should go, not become a setting.

---

## 5. What to add — ranked

### Tier A — ship now, **no engine work**, real user value

**A1 · The Settings page itself.** A real page, rendered for `tab === 'Settings'`
(`App.jsx:107`), using the existing `PropertySection` / `PropertyGroup` /
`PropertyRow` / `PropertyToggle` / `PropertySlider` / `PropertyDropdown`
components and the existing `.pl-*` styling, so the playlist dialog and the new
page look like one product. Sections below.

**A2 · Picture group in the Inspector: brightness / contrast / saturation.**
The cheapest real feature in this document. The engine applies all three live
(§2); the app just never sends them. Wiring: `main.cjs:239` (map) + `:628`
(`wallpaper:update` payload) + `desktop.js:31-38` (capability list) +
`wallpaperSettings.js:42-49` (persist) + a new Picture group in the Inspector.
**Video only** — the static adapter deliberately excludes colour effects
("no D3D12 pipeline yet", `RendererAdapters.cpp:50-52`), so the group must be
capability-gated exactly like the audio-track probe. Range 0…2 with 1.0 = identity
(the engine's clamp, `Engine.cpp:1085/1103/1121`).

**A3 · Diagnostics section.** `GET_STATUS` already returns render FPS,
scheduler drops, A/V drift in ms, VRAM (dedicated/shared/budget), HDR state,
display max nits, present-gap percentiles and the occlusion-park flag
(`wallpaper_cli_main.cpp:1342-1412`). There is **no `wallpaper:status` IPC**
(the string does not appear anywhere in `app/electron/`). One new IPC + one
panel turns the engine's own instrumentation into something a user can read —
and it is the honest way to answer "is this wallpaper costing me GPU?".

**A4 · Window position & size persistence.** The `window` group exists in
`app-state.json` (`app-state-manager.cjs:20`) and **is never written** — the
window is hard-coded to 1280×800 at creation (`main.cjs:305-306`) and always
opens at the same spot. Every competitor remembers it.

**A5 · Close behaviour (close to tray vs quit).** `tray.closeToTray` and
`tray.minimizeToTray` exist as state (`app-state-manager.cjs:36`) and are
never read: the close handler *always* hides and destroys the window while the
app survives in the tray (`main.cjs:361-373`). A user who wants the app to
actually quit cannot.

### Tier B — small, bounded engine work, then app work

**B1 · Live "pause when a fullscreen app is in front" (on/off).** The engine
implements the detector and the switch (`Engine.cpp:1824`, `WallpaperConfig`
`:115`) and the runtime already accepts the INIT parameter (`:909`) — but
there is no live setter, so today the behaviour is **permanently on** and
uncontrollable. Both competitors make this a headline setting. Needs: a
`SET_PAUSE_ON_FULLSCREEN` command (or an atomic read of the flag — `m_config`
is read from the render thread, so it must not be a plain bool) + an Inspector
/tray control. **Note:** `TODO.md` P3-1 ("unified playback-policy layer") is the
right owner for the *policy* side; this is the one lever it needs.

**B2 · Live frame-rate cap.** The user cap exists in the cadence policy
(`CadenceController.hpp:21-35`) and is read per frame from `m_config.maxFps`
(`Engine.cpp:2533, 2552`) — but that read is currently **unsynchronised**
(the P3-4 spec says so explicitly: *"the live frame-rate cap must be
race-free — a dedicated atomic"*, `2026-10-20-performance-modes-design.md`
§8). P3-4 owns the design; B2 is the one command it needs. Until then the
honest UI is a *pending* label, exactly as that spec prescribes.

**B3 · Live rotation / flip.** The engine implements it, both adapters
advertise it, and the dispatcher is missing one branch (§2). One branch plus
app plumbing.

### Tier C — real competitor gaps, but they need design first (do not fake)

**C1 · Per-app rules** ("pause when Steam.exe runs", "mute when a recorder is
capturing"). Wallpaper Engine's Application Rules and Lively's pause
algorithms both exist. REX has the fullscreen detector and the power/lock
suspend sources, but no foreground-app watcher and no rule model. This is
**P3-1's** job in `TODO.md` — a policy layer that owns play/pause/mute/unload
from app + monitor + power state, with per-app rules as one input. Doing it
without that layer produces a second, competing policy engine.

**C2 · Multi-monitor selection and profiles.** `TODO.md` P3-2. The engine
holds EDID-keyed per-monitor assignments, but `SET_MONITOR` cannot move a
wallpaper and there is no UI.

**C3 · Screensaver mode.** Both competitors ship one. Needs a Windows
`.scr` integration — a project of its own, out of scope here.

**C4 · HDR policy (follow Windows / force SDR).** The engine has the shader
path (`D3D12Renderer.cpp:2586-2587`) and a spawn flag, but no protocol
command — a *load-time* lever, exactly as P3-4 §4.2 frames it.

**C5 · Suspend policy (session lock / display off / battery).** Currently
hard-coded to "pause" (`Engine.cpp:1925-1957`). P3-4 §4.3 wants
pause / stop (free VRAM) / keep running as an app-level setting.

---

## 6. What NOT to build

- **Do not surface the registry.** ~134 keys, 6 live. A Settings page built
  from the model is the Inspector's old failure mode in a new place.
- **Do not build a theme picker** — the palette is deliberately single
  (§4.5).
- **Do not expose `automation.*`** — its signals are simulated (§4.4).
- **Do not expose the playlist filters that read placeholder data** (§4.3)
  until the data source is real (the file facts the app already probes —
  size, audio track — are the honest starting point).
- **Do not ship a multi-monitor picker** before `SET_MONITOR`/placement works
  (C2); a picker that cannot move the wallpaper is theatre.
- **Do not add a WPE-style per-wallpaper property system** — REX content is
  plain video/image files; nothing declares properties (already settled in
  `research-inspector.md` §5).

---

## 7. Measurement plan (make "most used" data, not opinion)

Add local-only counters to `app-state.json` (`usage.settings.<control>`), no
network, resettable from Diagnostics. Count: settings page opens, each
control changed, each picture adjustment committed, pause-on-fullscreen
toggled, diagnostics panel opened, and — the one that matters for ranking —
per-control **use vs. mere presence**. After ~2 weeks, prune against the
table in §5. Until then the ranking is competitor- and
support-theme-derived, not measured.

---

## 8. Open decisions

1. **A5 vs. the tray's existing "Launch at login".** If the Settings page
   owns startup behaviour, the tray checkbox becomes a mirror of it (or is
   removed) — two homes for one preference is how the app got here.
2. **A1 layout.** One scrolling column of sections (like the Inspector), or a
   two-column page (sections left, content right)? The playlist dialog is
   single-column at 380 px; the app page has ~1 200 px and could carry a
   sidebar of sections.
3. **B1 ownership.** Is "pause on fullscreen" an engine lever (a `SET_`
   command) or a policy input of the P3-1 layer? Doing it as a `SET_` command
   first is cheaper; doing it inside P3-1 avoids two owners.
4. **Diagnostics depth.** A read-only panel of engine telemetry, or also
   controls (e.g. force a runtime restart, clear the crash-report folder)?
5. **Where does the "pause policy" UI live** — global Settings (applies to
   everything) or per-wallpaper (this file only)? Both competitors have both.

---

## Evidence index

- Runtime commands: `rex-core/src/wallpaper_cli_main.cpp:833-1444`; SET block
  `:1122-1180`; INIT `:889-966`; GET_STATUS `:1325-1427`; GET_CAPABILITIES
  `:1429-1441`; `--force-sdr` `:2505`.
- Live property paths: `rex-core/src/wallpaper/engine/Engine.cpp:1065-1290`
  (fillMode, volume, speed, loop, mute, brightness, contrast, saturation,
  audioMode, rotation, flipH, flipV); per-frame colour constants
  `rex-core/src/graphics/renderer/D3D12Renderer.cpp:2589-2593`, b0 budget
  `:1937-1940`; adapters `rex-core/src/wallpaper/runtime/RendererAdapters.cpp:53-87`.
- Fullscreen + suspend policy: `Engine.cpp:1824-1874` (fullscreen),
  `:1925-1957` (lock / display-off / power), `rex-core/src/wallpaper/engine/PowerPolicy.hpp:15-20`.
- Config surface: `rex-core/include/rex/wallpaper/IWallpaperEngine.hpp:103-119`.
- Cadence: `rex-core/src/wallpaper/playback/CadenceController.hpp:21-35`;
  unsynchronised read `Engine.cpp:2533, 2552`.
- App forwarding: `app/electron/main.cjs:239` (property map), `:570-624`
  (apply), `:628-674` (live update), `:676-679` (capabilities), `:589` (INIT);
  `app/electron/wallpaperRuntime.cjs:186-193`.
- App capability filter: `app/src/wallpapers/desktop.js:31-38`,
  `app/src/wallpapers/capabilities.js:7-18`.
- Persistence: `app/electron/app-state-manager.cjs:17-41` (default state),
  `app/src/wallpapers/wallpaperSettings.js:42-49` (the six live paths).
- Dead surface: the eight `app/src/wallpapers/inspector/sections/*` files not
  imported by `Inspector.jsx:12-16`; `registry.js:173-402` (the model);
  `PlaylistEngineDialog.jsx:409-413` + `engine.js:230` (multiMonitor);
  `engine.js:385-413` (filters on placeholder data);
  `engine.js:766-815` + `engine.js:837-838` (automation on simulated signals).
- Stub Settings page: `app/src/App.jsx:21-31, 102-125`;
  `app/src/components/SidebarLayout.jsx:28-34`;
  `app/electron/tray-manager.cjs:99-102`.
- References: Wallpaper Engine *Fix issues with specific apps or games*
  (application rules), *Select wallpapers per application* (profiles);
  Lively *Performance* (pause algorithms, GPU decode) and *Video Guide*
  (player, GPU decode); Windows Personalization (fit/fill/span/tile/center).
