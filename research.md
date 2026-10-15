# REX — Stage Research (2026-10-15)

Four read-only research tracks covering the stages the stutter/color/audio
campaign did **not** freshly investigate. Each track states DONE / FOUNDATION /
GAP per item with file:line evidence. No code was changed to produce this file.

Prior audits referenced: 2026-08-24 gallery audit, 2026-10-06 parallel audits,
2026-10-10 architecture reference, `wall_research.md`, `_research_audio.md`,
`_research_present_pacing.md`, `docs/bugs/BASELINE-2026-10-15-phaseA.md`.

**How to read the verdicts:** DONE = works end-to-end today. FOUNDATION = code
exists but is not wired end-to-end. GAP = missing or broken. WIRED-UNVERIFIED =
connected but never proven live. PARTIAL = chain has a break.

---

## Track 1 — Gallery / media import (fresh audit; Aug-24 audit is stale)

Actual current files: tiles/import/restore live in
`app/src/wallpapers/WallpaperGallery.jsx` (816 lines); `gallery.js` is now only
a thin bridge + record builder (`app/src/wallpapers/gallery.js:1-4`); thumbnail
helpers `app/src/wallpapers/thumbnail.js:9-96`; dedup + defaults
`app/src/wallpapers/registry.js:45-65,200-214` + `store.js:139-180`;
persistence `app/electron/wallpaper-library.cjs:7-227`,
`preload.cjs:44-51`, `main.cjs:470-493,522-539,622-626`. No `widgets/`
directory exists and nothing in `app/src` imports from it — the other session's
refactor does not break this path.

| Area | Verdict | Key evidence |
|---|---|---|
| Thumbnails | **GAP (broken on fresh import)** | Two stacked bugs: (1) `gallery.js:49` calls `createThumbnail(f, thumbUrl)` but the signature is `createThumbnail(file, maxSize=480, urlOverride=null)` (`thumbnail.js:90`) — the URL lands in `maxSize`, so fresh import (`null`) yields a 1×1 canvas and restore-with-URL yields `NaN` canvas math. (2) `thumbnail.js:23-54` resolves a bare `dataUrl` string, but `gallery.js:51` reads `thumb?.dataUrl` — always `null`, so `saveThumbnail` never fires and tiles fall through to the icon fallback (`WallpaperGallery.jsx:606-614`). The restore progressive path (`WallpaperGallery.jsx:197-203` → `createThumbnailFromUrl`) is correct — which is why restored tiles *can* show posters but fresh imports cannot. |
| Thumbnail persistence | **GAP** | Write exists (`thumbnail.js:14-17` → `preload.cjs:32` → `main.cjs:470-484`, `${userData}/thumbnails/<uuid>.webp`) but **nothing ever reads it back**: `thumbPath` is written (`gallery.js:52-68`) and deleted (`WallpaperGallery.jsx:117-119`) but never loaded; the library `thumbnail` field (`wallpaper-library.cjs:96,103`) is never read by the renderer (`gallery.js:83-98` ignores `item.thumbnail`); restore mints a fresh UUID (`gallery.js:50`) and re-decodes from `file://` every boot (`WallpaperGallery.jsx:194-208`). Regenerated every launch — the Aug-24 finding still holds. |
| Path persistence | **DONE** | Per-entry `id/type/path/title/thumbnail/metadata/tags/favorite/addedAt/lastUsed` (`wallpaper-library.cjs:98-109`), atomic flush (`:33-41`), path-normalized index (`:43-54`), bridge intact (`gallery.js:8-38` → `preload.cjs:44-51` → `wallpaper-library.cjs:212-224`). |
| Playback/thumbnail persistence | **GAP** | `DEFAULT_PLAYBACK` (`registry.js:200-214`) is spread fresh on every import (`registry.js:392`); `thumbDataUrl`/`thumbPath` are in-memory record fields only (`gallery.js:67-68`). |
| `gallery.read-file` | **GAP / breakage risk** | **Two handlers register the same channel:** `main.cjs:522` (returns byte-copy `{ok,dataUrl,…}`, 150 MB cap) and `wallpaper-library.cjs:221` (returns metadata-only `{ok,path,url(file://),…}`). Electron throws on a second `ipcMain.handle` for one channel — startup may crash, swallow, or last-win; unproven without running. The consumer (`gallery.js:83-98`) expects the **library** shape; if main's handler wins, restore loses `url/type/path`. Most urgent read-only finding on this track. |
| Import dedup | **DONE** (edited-file refresh GAP) | Same-session `fileIdentity` + `splitNewFiles` (`registry.js:45-65`); cross-restart path-hit → `persistedDupes`, bumps `lastUsed`, no duplicate (`WallpaperGallery.jsx:258-301`, `wallpaper-library.cjs:57-73`). Edited file keeps the old poster (no size/mtime comparison, no thumbnail refresh). |
| Startup restore | **DONE** (100-cap) | Restores tiles only, never prunes (`WallpaperGallery.jsx:158-213`); missing files skipped, entries kept (`:164-168,180-182`); `pathHint` preserved (`gallery.js:66`, `store.js:154-159`). Libraries >100 entries silently truncate tiles (`:164`). |
| Favorites | **GAP** | Stored (`wallpaper-library.cjs:106,147`) and inspector-toggled (`InfoSection.jsx:55`) but zero gallery UI — no toggle, no filter. |
| Filtering/search | **GAP** | Sort-only gallery (`WallpaperGallery.jsx:29-34`); backend already supports `type/favorite/search/sortBy` (`wallpaper-library.cjs:143-161`) but the sole call passes `{sortBy:'lastUsed'}`; title-bar search is decorative (no value/onChange). |
| Missing-file handling | **DONE** | Skipped, kept in library, never pruned; only explicit `removeFile` deletes. |
| Virtualization | **GAP (dead code)** | `GalleryGrid.jsx:1-73` implements windowing but has zero importers; the gallery renders `sortedFiles.map` directly. |
| Post-refactor wiring | **DONE (intact)** | `App.jsx:4-6,25,28` mounts gallery + inspector; no widget imports; Settings/About panels are empty `<h2>` (coverage gap, not breakage). |

---

## Track 2 — WorkerW / desktop-layer lifecycle

All lifecycle semantics VERIFIED in current code. `MonitorCompositor` window
threading is the one OPEN code gap; `docs/` §8 retains pre-fix diagrams.

| Semantic | Verdict | Key evidence |
|---|---|---|
| UnloadScene unconditional on image path | **VERIFIED** | `wallpaper_cli_main.cpp:911-925` calls `engine->UnloadScene()` with no state check; `Engine.cpp:606-657` has no state guard (safe). Video path stays state-gated (`:952-964`) to satisfy `LoadScene`'s `Initialized\|Stopped` guard (`Engine.cpp:434`). |
| Hide-on-unload / show-on-load | **VERIFIED** | Contract `IDesktopHost.hpp:17-21`; `SW_HIDE` in `WorkerWManager.cpp:251-260` + `ProgmanHost.cpp:346-355` (both window-thread-marshalled, pre-Acquire no-ops); delegates in `WorkerWHost.cpp:93-95`, `MultiMonitorDesktopHost.cpp:65-67`. Hide at `Engine.cpp:644-646`, show at `:598-600` (any `LoadScene`, broader than docs' "video/audio" wording — superset). Tests in `tests/test_desktop_host.cpp:304-333`. |
| PAUSE holds frame + position | **VERIFIED** | `Engine.cpp:688-718` (requires Running, no clear/hide); decode thread parks (`VideoPlaybackController.cpp:453-457`); render loop stops ticking when not Running (`Engine.cpp:1882-1887`); audio pause retains (`AudioPlayer.cpp:272-274`). |
| STOP resets position, keeps frozen snap | **VERIFIED** | Position zeroed twice (`Engine.cpp:730-731`, `VideoPlaybackController.cpp:199-234` incl. decoder seek-to-0 + `Clear()+ResetPresentationClock()`); re-present disarmed (`Engine.cpp:732-734` `m_hasVideoFrame=false`); no hide/clear so the last backbuffer stays visible. |
| UNLOAD hides layer | **VERIFIED** | `Engine.cpp:606-657` — Close, ClearVideoFrames, `ClearTexture()+ClearVideoFrame()`, `SetVisible(false)` (`:644-646`), recovery disarmed (`:650-651`), state Initialized. |
| SHUTDOWN releases | **VERIFIED** | `Engine.cpp:222-280` — thread join, compositor shutdown, audio/renderer/adapter reset, `m_desktopHost->Release()`; window destruction on the window thread (`WorkerWManager.cpp:200-246`, `ProgmanHost.cpp:312-344`). |
| Recovery disarm on unload | **VERIFIED** | `Engine.cpp:647-651` clears `m_recoveryNeeded`; consumer at `:1821-1854` with backoff ladder (`RecoveryBackoff.hpp:24-30`, 250 ms → 5 s cap). Only `UnloadScene` disarms (STOP keeps scene, LOAD wants recovery — by design). |
| MonitorCompositor window threading | **OPEN** | Create/destroy/move run inline on the caller thread (`MonitorCompositor.cpp:56-61,70-71,81,100,177,187`) with no `DesktopWindowThread`; call sites span INIT thread (`Engine.cpp:190`), render thread (recovery `:355`, hot-plug `:1528-1553`). Presents are render-thread-consistent (`Engine.cpp:2014-2016,2032-2034`) — the gap is lifecycle ownership only, not the present path. |
| Docs vs code | **§9 correct, §8 drifted** | `docs/wallpaper-engine-architecture.md` §9 matches code; §8 retains pre-fix diagrams routing UnloadScene to "Frozen" (`:231-251,327-334`), a stale `any→UNLOAD` matrix row (`:321`), and stale line refs throughout. |

---

## Track 3 — Multi-monitor features

| Feature | Verdict | Key evidence |
|---|---|---|
| Different images per monitor | **FOUNDATION** (engine works, app never calls it) | `VideoManager.cpp:389-430` `SetPerMonitorWallpaper()` → per-compositor `PresentStaticTexture`; CLI `LOAD{"monitor"}` (`wallpaper_cli_main.cpp:993-1005`); test `test_multi_monitor.cpp:65-95`. No Electron caller. |
| Different videos per monitor | **FOUNDATION** | One controller per `MonitorEntry` (`VideoManager.hpp:255-261`, `.cpp:535-542`, shared D3D11 context + fence); `PresentToAllMonitors` prefers per-monitor `Tick(0.0)` (`:367-387`). **Zero IPC callers** of `OpenVideoForMonitor`; `LOAD` video opens only the shared controller (`Engine.cpp:487-495`). |
| Shared vs per-monitor decode | **FOUNDATION** (both exist; shared is live) | Shared `m_controller` (`VideoManager.hpp:226`, opened `:108-152`); per-monitor controllers share only context+fence (`:539-542`). Only the shared path is driven by the render loop. |
| Audio ownership across monitors | **FOUNDATION** | First-opened monitor video keeps audio, later ones muted (`VideoManager.cpp:554-567`); tested (`test_multi_monitor.cpp:141-182`). No UI/policy to elect owner, no ducking. |
| EDID identity | **DONE** (derivation) | `refreshHz` via `EnumDisplaySettingsW` (`Engine.cpp:856-864`), `edidHash` = FNV-1a over registry EDID bytes (`MonitorEdid.cpp:10-61`, `Engine.cpp:867`), `adapterLuid` via DXGI (`:874`); `AssignmentKey()` falls back to `\\.\DISPLAYn` id when hash is 0 (`VideoManager.cpp:252-262`). |
| Native per-monitor persistence | **FOUNDATION** | EDID-keyed `monitor.wallpaper.<hex>` via `ISettingsService` (`VideoManager.cpp:252-262,411-425,436-460`), restored on INIT. A parallel `RuntimeSettings` monitor API has no production callers. |
| App-side persistence | **GAP** | Electron persists ONE wallpaper (`main.cjs:111`, `startup.cjs:20-31`); reapply/boot paths send no `monitor` field; supervisor store is single `lastWallpaper/monitorId:null`. No EDID-keyed map in `app/`. |
| Hot-plug reconcile | **FOUNDATION** (reconcile exists; restore incomplete) | 120-frame poller (`Engine.cpp:1796-1811`), volatile-id predicate (`DisplayTopology.hpp:29-43`), handler touches no media (`Engine.cpp:1473-1568`). Media survives on remaining monitors. |
| Reconnect survival | **GAP** | `RemoveMonitor` erases the entry incl. its assignment (`VideoManager.cpp:332-348`); re-added monitors start blank (`:305`) and never consult settings; reconcile keys on volatile `DISPLAYn` id, not `edidHash`. |
| Saved monitor profiles | **GAP** | None. `ProfileManager` holds a single `wallpaperId` and `applyProfile` never calls the wallpaper engine. Playlist "multimonitor options" are dialog-only labels. |
| Mixed-GPU detection | **DONE** (telemetry only) | Decoder/renderer/display LUIDs compared (`VideoManager.cpp:162-196`, `Engine.cpp:1224-1233`), surfaced in telemetry + CLI. |
| Mixed-GPU handling | **GAP** | Construction assumes one adapter (`VideoManager.cpp:28-44`); compositors reuse the single D3D12 device; no branch on mismatch, no cross-adapter copy. |
| UI pickers | **FOUNDATION** (renders, store-only) | `PropertyMonitorPicker.jsx:3-22`, `MonitorSection.jsx:16-32` write to an in-memory Map (`store.js:52-61`). |
| UI→runtime apply | **GAP** | `desktop.js:39-108` sends no monitor identity; preload exposes no monitor API; `DesktopApply.jsx` never keys on monitor. Pickers never reach `LOAD.monitor` / `SET_MONITOR` / `OpenVideoForMonitor`. |

---

## Track 4 — Recovery and supervision wiring

Scale: WIRED+VERIFIED = code + connected + proof; WIRED-UNVERIFIED = connected, no live proof; PARTIAL = break in chain; MISSING = absent.

| Component | Verdict | Key evidence |
|---|---|---|
| Supervisor as single exit driver | **WIRED-UNVERIFIED + PARTIAL** | Constructed + started in `main.cjs:636-661`; old inline `recoverRuntime` gone (no double-reapply); `resetRecovery()` on deliberate applies (`main.cjs:198,363,406`); `stop()` on quit (`:715-717`). Headless 12/12 vs fake native (`tools/verify/sup_wire.cjs`). **Live crash test pending.** PARTIAL: `supervisor-state.json` is dead — nothing ever calls `stateStore.update()`; real cross-restart persistence is `startup.cjs` `last-wallpaper.json`, not the supervisor file. |
| Watchdog / heartbeat | **PARTIAL** | Runtime heartbeat consumes correctly (`runtime-manager.cjs:31-39`, `supervisor.cjs:128-152`) but is **parked live** (`main.cjs:659` forces `missLimit: Infinity`). Renderer watchdog input disconnected: `recordRendererPing()` has zero callers in `app/`; no `renderer-ping` IPC, no `render-process-gone` handler. Both inert by design/omission, guards keep them silent. |
| Native PING | **PARTIAL** | Side-effect-free impl present (`wallpaper_cli_main.cpp:790-796`, before INIT/LOAD branches); consumption wired (`runtime-manager.cjs:34`). Gaps: linked binary may predate it; a checked-in script asserts `pong.pong===true` but native sends null data — that check can never pass. |
| Poison-file breaker | **WIRED-UNVERIFIED to IPC, MISSING toast** | Parks after 3rd consecutive crash on same path (`supervisor.cjs:209-224`); manual apply resets (`:87-92`); park reaches renderer IPC (`:273-275` → `main.cjs:646-651` → `preload.cjs:23-27`) but `runtimeEvents.js:19-24` toasts only `ERROR`/`recovery-gave-up`, **not** `recovery-parked-poison`. Headless poison proof claimed but no poison harness exists in repo. |
| AppStateManager | **WIRED-UNVERIFIED + latent bug** | Instantiated in `whenReady` (`main.cjs:624`); versioned file + atomic tmp+rename + daily backups + corruption recovery (`app-state-manager.cjs:17-150`); headless round-trip proof (`tools/verify/p03_wire.cjs:44-69`); live pass pending. **Latent crash:** `set()` on a never-existing nested key throws `TypeError` (`:177-186` walks without creating intermediates; `get` is safe). |
| Library self-registration | **PARTIAL (registration conflict)** | Self-registers (`wallpaper-library.cjs:8-14,211-224`), held at `main.cjs:625`. But `main.cjs:522-539` **also** registers `gallery.read-file` with an incompatible byte-copy contract vs the library's metadata-only contract (`:181-209`); the consumer (`gallery.js:83-98`) expects the library shape. Second `ipcMain.handle` on one channel throws — live startup behaviour unproven. |
| Engine recovery state machine | **WIRED-UNVERIFIED** | WorkerW path re-verifies before success (`WorkerWManager.cpp:681,698`, 7 invariants `:367-593`, `TryReanchorLayer` `:270-316`); engine preserves scene+position across device loss (`Engine.cpp:368-428`); backoff ladder (`RecoveryBackoff.hpp:24-30`); unit proofs (`RecoveryBackoffTest 4/4`, policy 7/7). **No live Explorer-kill/device-loss run cited.** Verification lives in `WorkerWManager`, not in `HandleDeviceLoss` itself. |

Live-pass checklists per item are in the track detail above (what to kill/observe/log for supervisor, heartbeat, PING, poison, AppState, library, engine recovery).

---

## Consolidated priority (what to do, in order)

Derived from all four tracks + the stutter/color/audio campaigns. P0 items unblock correctness; nothing below starts until the P0 above it is verified.

**P0 — correctness and safety**
1. **Scheduler live A/B on the 2x file** (`marvel-spiderman-miles-morales.1920x1080.mp4`, fix `e5bfdf0` vs `bcdfcd6`): require no content advance beyond playhead + tolerance, `effFps` at source rate, no freeze on loop wrap. Unit tests prove logic; only live timing proves playback.
2. **Content-rate check** (displayed-PTS advance vs wall clock in `GET_STATUS`): the blind spot that hid the 2x bug behind healthy `1.000x`/`effFps` telemetry. Read-only first.
3. **`decodeMsP50/P95/P99` into the text telemetry line**: exists in JSON only (`wallpaper_cli_main.cpp:1298-1300`); pure plumbing that answers "is decode itself slow?" without probe builds.
4. **Resolve the `gallery.read-file` dual registration**: keep the library metadata-only contract + enforce the size cap there; delete `main.cjs:522-539`. One-line-class fix, but startup may currently throw on it — verify live boot first.
5. **Thumbnail builder argument/return-shape bugs** (`gallery.js:49,51` vs `thumbnail.js:90,23-54`): fresh imports produce 1×1/`NaN` posters and nothing persists. Fix the call shape, then the persistence read-back.

**P1 — reliability**
6. **WorkerW hide-on-UNLOAD live verification** (code verified; needs video→image 10 s watch, image→video→image, PAUSE→PLAY, STOP→PLAY, shutdown).
7. **`MonitorCompositor` window-thread ownership** (the one OPEN lifecycle gap): marshal create/destroy/move like `WorkerWManager`/`ProgmanHost`, or document why the caller-thread affinity is safe.
8. **Supervisor + AppState + library live pass** (kill runtime → 1 reapply; 4 kills → park; quit-during-crash → silent; reboot → boot re-apply; corrupt app-state → quarantine + restore; poison file → 2 reapplies + park + toast after adding it to `runtimeEvents.js`).
9. **Un-park heartbeat + renderer-ping bridge** (needs the PING binary linked + `pong.pong` assertion fixed + `renderer-ping` IPC + `render-process-gone` handler), or explicitly keep parked with the reason recorded.
10. **Reconnect survival**: `AddMonitor` should consult settings / restore the assignment instead of starting blank; stop keying reconcile on volatile `DISPLAYn` id.

**P2 — compatibility**
11. **Capability-aware decode fallback** (probe → HW → SW → explicit error); SW AV1-HDR `-40` root-cause (build lacks a working SW AV1 decoder vs REX misconfiguration); poison-file guards stay.
12. **HDR/vendor matrix** (NVIDIA, HDR-on, multi-GPU) — no colour code without a live HDR reference.
13. **Docs drift**: redraw `docs/wallpaper-engine-architecture.md` §8 pre-fix diagrams/rows, re-base stale line refs; fix `wallpaper_cli_main.cpp:495-500` protocol comment (missing PING).

**P3 — product policy**: unified playback-policy layer; multi-monitor independent selection end-to-end (engine supports images per monitor today — wire app→IPC→`LOAD.monitor` first, videos second); FPS counter un-hardcode; audio benchmark + A/V sync design.

**P4 — deferred**: Web/Scene/Shader/AudioVisualizer apply paths, SDK, marketplace, automation, themes; widgets/taskbar (other session active — do not touch `app/src/widgets/`).

---

## Precautions (binding on all of the above)

1. Validate every metric before trusting it (retired this campaign: `DisplayedTime`, AMD `VDec`, cumulative `PresentRefreshCount` as per-frame, `effFps`-means-smooth).
2. Controlled conditions: load <20%, SDR-only clips (post-blank protocol), graceful stops, drops+repeats as window deltas, 5–10 runs (variance is bimodal).
3. One change per commit, 689/689 gate, live A/B on the problem file before calling behaviour done.
4. Do not touch: colour code, `app/src/widgets/`, the 8-slot pool size, `maxBurst` above 1.0 (held off pending the scheduler landing live).
5. Feature flags for rollback (`REX_DECODE_AHEAD` pattern).
