# Paiint (formerly REX) Project Status

> Last updated: 2026-10-27
> Auto-generated from codebase audit. Update this file when a feature reaches ✅.
> See also: [everything.md](everything.md) (done inventory) · [TODO.md](TODO.md) (remaining work).
> Note: the "Shell UI" section below is **retired/historical** — the native C++ shell was archived in `dump/` (and the orphaned `wallpaper/renderer/` subsystem was archived there on 2026-10-19). The current UI is the Electron app in `app/`; statuses inside that section are snapshots from its July audit and are not maintained.

## Recent changes since the July audit

**2026-10-27 — MSIX lifecycle fixes and a quit drain (✅ dev verified; packaged smoke test blocked).** Three packaged-only defects were fixed and one quit bug found while testing them. (1) **Stills:** `SetImageWallpaper` wrote `HKCU\Control Panel\Desktop\Wallpaper` + `SystemParametersInfoW`, both invisible to Explorer under MSIX; it now uses `IDesktopWallpaper::SetWallpaper(NULL, path)` (shell COM, unvirtualized, fit modes mapped to `DESKTOP_WALLPAPER_POSITION`) and keeps the registry/SPI pair only when `GetCurrentPackageFullName` reports no package identity. (2) **Snapshot:** the last frame moved from the virtualised `%LOCALAPPDATA%\Paiint` to the real Pictures known folder — `SHGetKnownFolderPath(FOLDERID_Pictures)` → `Pictures\Paiint\last-frame.png`, mirrored in Electron via `app.getPath('pictures')` — so Explorer can read it and uninstalling cannot orphan the live wallpaper. (3) **Launch at login:** the manifest now declares `desktop:Extension Category="windows.startupTask"` (`TaskId="PaiintStartup"`, `Enabled="false"`) backed by a new `rex-core/tools/StartupStub.cpp` → `paiint-startup.exe`, the only process with the package identity needed to drive `Windows.ApplicationModel.StartupTask` (`--status/--enable/--disable`, bare launch → `Paiint.exe --startup-task`). `app/electron/startup-task.cjs` (6 tests) bridges it, caching the answer for 10 s so tray rebuilds never block on a spawn; `applyLoginItem`/`loginItemEnabled` prefer the task and fall back to the Run key when it reports `unpackaged`, and `--startup-task` honours the live `startup.startMinimized` (tray-only when true, window when false). (4) **Quit drain (P0 found while testing):** `will-quit` sent `SHUTDOWN {snapshot:true}` and returned, so Electron tore the runtime down mid-capture instead of waiting for it to finish. `wallpaperRuntime.stop()` now resolves on the child's exit or a bounded deadline (15 s snapshot / 5 s plain), and `will-quit` prevents default, awaits it, then re-quits through a `setImmediate` macrotask behind `quitDrainStarted`/`quitDrainFinished` flags (a resolved stop settles as a microtask inside the emission, where a re-entrant quit can be dropped; a second quit during the drain waits instead of killing the capture), with the pre-drain cleanup exception-guarded. **Dev verified:** `IDesktopWallpaper` apply logged and the registry agreed; a quit wrote a valid 3840×2160 `Pictures\Paiint\last-frame.png` and left the stale AppData copy untouched; `--startup-task` produced `window: deferred (startup task, start minimised)` with no page (and a window with `startMinimized=false`); the Run key gained/rewrote/lost its `--background` flag with the preference; three closes took 1.6–3.2 s with runtime exit 0 and `app: runtime drained, quitting`. `startup-task` tests 6/6, app suite **100/100** (68 main-process + 32 renderer), lint 0, typecheck 0, build 0; `build-msix.ps1` validates the stub and every extension executable. **Blocked:** the same operations inside an installed package — sideloading needs admin or Developer Mode, neither available here. `docs/legal/PRIVACY.md` now lists the Pictures snapshot path.

**2026-10-26 — playlist scoping, Stop, quit-snapshot toggle and verified recovery (✅ live verified).** Playlist rules now apply only to playlist members: `app/src/wallpapers/playlist/scope.js` resolves the governing playlist from the store's **applied record** (falling back to `lastAppliedPath` only at boot), so applying a gallery file outside the playlist clears main's rotation plan instead of letting the old Timer/scene-end rules keep firing over it (8 unit cases; live: a Nature plan cleared on a non-member double-click). The dock's new **Stop** button kills the video and leaves the last frame as a still: `playlistEngine.stopRotation()` clears the local plan, marks the applied file's playlist as an explicit `'None'`, mirrors `syncPlaylistPlan(null)`, then `SHUTDOWN {snapshot:true}`; main also clears its in-memory `lastApplied` so a reopened window cannot re-adopt the stopped scene, while `last-wallpaper.json` still restores on a full restart. Found live and fixed: explicit `'None'` used to resolve to the real `pl-0` sentinel, which handed the engine default Timer settings and restarted rotation after Stop. **Live:** plan null, stop button gone, 3840×2160 snapshot (11.7 MB), registry wallpaper path set, runtime exited 0; after `Page.reload` the plan stayed null. A Settings → Engine toggle ("Leave the last frame on exit", `engine.snapshotOnQuit`, default on) now controls the quit snapshot and persisted `true → false → true` live. **P1-2 closed:** `app/electron/recovery.cjs` (6 tests) + a shared `replayStoredEntry()` restore the file, library settings, colour/playback state and scheduler, and the supervisor reports `recovery-ok` only after `GET_STATUS` proves the desktop layer (live: killed runtime → verified → `recovery ok (attempt 1)`, state 3, position advancing). Suite 95/95, lint 0, typecheck 0. Also fixed: dev launches preferred the stale `rex-core/.build` runtime (no snapshot support) over `rex-core/build`.

**2026-10-26 — release surface shipped: MSIX pipeline, About page, legal documents, JS licences (✅ built and verified).** `packaging/msix/` now holds a repeatable Store build (`AppxManifest.xml` with version/publisher tokens, `build-msix.ps1`, `README.md`, generated `Assets/`); it stages the electron-builder payload, generates the seven tile PNGs from `app/assets/paiint-app.ico`, validates the expanded manifest, and packs with `makeappx`. A full run produced `Paiint-0.1.0-x64.msix` (174.9 MB, SHA256 `76854DAD…9ED315DF`) whose unpacked contents include `Paiint.exe`, `resources\bin\paiint-wallpaper-runtime.exe` (hash-identical to the built runtime, so it carries the quit-snapshot and VEH fixes), all seven assets and `resources\legal\{EULA,PRIVACY,THIRD_PARTY_NOTICES,THIRD_PARTY_LICENSES_JS}.md` + `LGPL-2.1.txt`. In the app, the sidebar's **About** page now shows the gold mark, version, publisher, licence, copyright and privacy statement, and opens the EULA, privacy policy, third-party notices and the generated JavaScript licence texts in an in-app dialog; About is no longer a section inside Settings. The renderer asks for an allowlisted key (`app/electron/legal-docs.cjs`), never a path; packaged builds read `resources/legal`, dev builds read `docs/legal`. `PRIVACY.md` was written (no collection, no network requests, local paths listed) and `THIRD_PARTY_LICENSES_JS.md` is generated from the real production tree — **188 packages, 403 KB, 0 without licence text**. Verified: `legal-docs` tests 5/5, app suite 81/81, lint 0, typecheck 0.

**2026-10-26 — quit-time snapshot shipped (✅ live verified).** Quitting Paiint now leaves the frame that was on screen as the desktop wallpaper: the runtime captures its own back buffer on the render thread, PNG-encodes it to `%LOCALAPPDATA%\Paiint\last-frame.png`, applies it through the native Windows API (`SystemParametersInfoW`), and only then tears WorkerW down. HDR displays render the capture frame through the existing SDR tone map so the PNG matches what the desktop showed. The capture is opt-in (`SHUTDOWN {snapshot:true}` from `will-quit`), so the idle reclaim after an image apply never overwrites the user's image. Root cause of the first failure was a D3D12 rule, not engine logic: textures cannot live on READBACK heaps — fixed with a readback buffer + `CopyTextureRegion` footprints. Verified: 3840×2160 PNG with real picture content, registry wallpaper path set, reclaim leaves the file untouched, 824/824 native tests. The clean-quit false `VEH exception 0x71A` (WTS unregister during RPC teardown) is fixed too: the VEH ignores that one code once a deliberate teardown has begun, and a snapshot quit left `crash.log` unchanged.

**2026-10-25 — product rename shipped (✅ live verified).** The app is now **Paiint 0.1.0**, authored by **MoiNeX**. Every user-visible `REX`/`Moinu` surface was scrubbed (window, tray, logs, IPC names, exports, icons); the native runtime ships as `paiint-wallpaper-runtime.exe`. Upgrades are lossless: `%APPDATA%\REX` / `%APPDATA%\pg` are copied into `%APPDATA%\Paiint` on first launch (pinned path), stale `REX*.exe` login items are removed, and the playlist storage key migrates in place. The runtime's diagnostics moved from `%TEMP%\REX` to `%TEMP%\Paiint` (five native sites) and its settings from `%LOCALAPPDATA%\REX` to `%LOCALAPPDATA%\Paiint`. Live: 35 library records, 1 playlist and the `After Current Ends` rotation (queue=11) survived; second launch is single-instance. Open for a public release: the MSIX packaging pipeline and the final logo. Code signing is no longer a blocker — a spike proved a packaged build still parents into Explorer's `WorkerW` and renders, and the Microsoft Store signs the package, so no certificate has to be bought.

**2026-10-24 — playlist rotation outlives the UI (✅ live verified).** The Timer and scene-end scheduler moved from the renderer to the main process (`app/electron/playlist-scheduler.cjs`, 13/13 unit tests). Closing the window now always destroys the renderer, and a `--background` login start rotates with no renderer at all. Live: two native-ENDED rotations with the window destroyed (`igris` → `KALYANI`, `reason=After Current Ends`), and reopening the UI adopted the rotated file with no second LOAD.

**2026-10-24 — decode path (✅ AMD verified, 🟡 Intel pending).** Hardware decode is now reported per stream (`hw=0|1`) instead of as a driver capability count, and the three silent software fallbacks log a reason. The `MiscFlags = SHARED | SHARED_NTHANDLE` decoder-device flag was removed (it applies to non-shareable decoder textures). AMD RX 6700 XT: H.264, AV1 and HEVC Main10 10-bit all `hw=1`, `content=1.00x`, `decP50=0.23–0.63 ms`. Intel UHD 770 re-test still needs one 4:2:0 10-bit control file. HEVC 4:2:2/4:4:4 remains software by design (no RExt GUIDs in the FFmpeg D3D11VA accel).

---

## Wallpaper Engine

### Renderers

| Feature | Status | Files |
|---------|--------|-------|
| Image wallpapers | ✅ | `wallpaper/renderer/ImageRenderer.hpp/.cpp` |
| Video wallpapers | ✅ | `wallpaper/renderer/VideoRenderer.hpp/.cpp` |
| Web wallpapers | ✅ | `wallpaper/renderer/IRenderer.hpp` (via `WebWallpaperPlugin`) |
| Shader wallpapers | ✅ | `wallpaper/renderer/ShaderRenderer.hpp/.cpp` — D3D12 root sig + PSO + HLSL |
| Scene wallpapers | ✅ | `wallpaper/renderer/SceneRenderer.hpp/.cpp` — ECS + RenderPipeline |
| Audio visualizers | ✅ | `wallpaper/renderer/AudioVisualizerRenderer.hpp/.cpp` — WASAPI + FFT |
| GIF/APNG support | ⬜ | |
| SVG wallpapers | ⬜ | |

### Library Management

| Feature | Status | Details |
|---------|--------|---------|
| Collections | ⬜ | |
| Favorites | ⬜ | |
| Tags | ⬜ | |
| Search | ⬜ | |
| Sort | ⬜ | |
| Filter | ⬜ | |
| Smart Collections | ⬜ | |
| Recent | ⬜ | |
| Trash | ⬜ | |
| Duplicate detection | ⬜ | |

### Playback

| Feature | Status | Details |
|---------|--------|---------|
| Pause on fullscreen app | ⬜ | |
| Pause on battery | ⬜ | |
| Pause on idle | ⬜ | |
| Pause on remote desktop | ⬜ | |
| FPS limit | ⬜ | |
| Background throttling | ⬜ | |
| Auto resume | ⬜ | |
| Multi-monitor sync | ⬜ | |
| Independent monitors | ✅ | `Engine::AssignMonitor`, `EnumerateMonitors` |
| Span wallpaper | ⬜ | |
| Mirror wallpaper | ⬜ | |
| Loop | ✅ | `Engine::SetLoop` |
| Speed control | ✅ | `Engine::SetSpeed` |
| Brightness/Contrast/Saturation | ✅ | `Engine::SetBrightness/Contrast/Saturation` |
| Volume | ✅ | `Engine::SetVolume` |
| Audio-reactive | ✅ | `Engine::EnableAudioReactive`, `SetAudioSensitivity` |
| Seek | ✅ | `PlaybackController::Seek` |
| Telemetry | ✅ | `PlaybackTelemetry` — framesDecoded/Dropped, FPS, latency |
| Playlist change modes (end of scene) | ✅ | `SceneEndLatch` / `EngineTelemetry::sceneEnded` + `ENDED {path, session}`; the app rotates the active playlist on the event (2026-10-22) |
| Playlist change modes (timer / dialog) | ✅ | the dialog edits the playlist the engine evaluates (`engine.activePlaylist()`); boot resolves `lastAppliedPath` with a retry, so modes run from a cold start (2026-10-22) |
| Playlist rotation without the UI | ✅ | `app/electron/playlist-scheduler.cjs` owns Timer + scene-end in the main process and restores the persisted plan at login; the renderer mirrors the plan and is destroyed on close (2026-10-23/24) |
| Playlist scoping + Stop | ✅ | the applied record decides membership (a non-member clears the plan); `stop` captures the last frame and leaves a still; explicit `None` never maps to the `pl-0` sentinel (2026-10-26) |

### Preview

| Feature | Status | Details |
|---------|--------|--------|
| Live preview | ⬜ | |
| Fullscreen preview | ⬜ | |
| Thumbnail generation | ⬜ | |
| Metadata | ⬜ | |

### Engine

| Feature | Status | Details |
|---------|--------|--------|
| LoadScene/UnloadScene | ✅ | `Engine::LoadScene`, `UnloadScene` |
| Play/Pause/Stop | ✅ | `Engine::Play`, `Pause`, `Stop` |
| Fill mode | ✅ | `Engine::SetFillMode` |
| D3D12 compositor | ✅ | `Compositor` in `graphics/compositor/` |
| Render thread | ✅ | Background render loop in `Engine` |
| Display change detection | ✅ | `CheckDisplayChanges` |
| Recovery | ✅ | `m_recoveryNeeded`, `ReinitializeCompositor` |
| MF decoder | ✅ | `MediaFoundationDecoder` in `wallpaper/decoder/` |
| WIC texture loader | ✅ | `WICTextureLoader` |
| Image loader | ✅ | `ImageLoader` |
| Scene loader | ✅ | `SceneLoader` |
| Frame snapshot | ✅ | `Engine::CaptureFrame` → `D3D12Renderer::CaptureFrameToPng` (readback buffer, WIC PNG) |

---

## Widgets (Rainmeter replacement)

### Widget Engine

| Feature | Status | Details |
|---------|--------|---------|
| Widget Host | ✅ | `WidgetHost.hpp/.cpp` — transparent layered HWNDs, WS_EX_LAYERED/TOPMOST/NOACTIVATE/TOOLWINDOW |
| Widget Runtime | 🟡 | `WidgetRuntime.hpp/.cpp` — stub (spawn/kill/update skeleton) |
| Widget SDK | ⬜ | |
| Widget API | ⬜ | |
| Widget Sandbox | ⬜ | |
| Widget Store | ⬜ | |
| Widget Installer | ⬜ | |
| JS bridge (postMessage) | 🟡 | `HandleWebMessage` in WidgetHost — close + getStats handlers, WebView2 init stubbed (MSWRL::Callback issue) |

### Built-in Widgets

| Widget | Status | Details |
|--------|--------|---------|
| Clock | ✅ | `widgets/clock/clock.html` — time display, double-click close |
| Weather | ✅ | `widgets/weather/weather.html` — Open-Meteo API, auto-refresh 30min |
| System Monitor | ✅ | `widgets/sysmon/sysmon.html` — CPU/GPU/RAM bars + NET up/down |
| Calendar | ⬜ | |
| Notes | ⬜ | |
| Music | ⬜ | |
| RSS | ⬜ | |
| Battery | ⬜ | |
| Calendar Agenda | ⬜ | |
| World Clock | ⬜ | |
| Process Monitor | ⬜ | |
| GPU Temp | ⬜ | |

### Desktop Widget Features

| Feature | Status | Details |
|---------|--------|---------|
| Drag | ⬜ | |
| Snap | ⬜ | |
| Align | ⬜ | |
| Lock | ⬜ | |
| Group | ⬜ | |
| Layers | ⬜ | |
| Opacity | ✅ | `WidgetInstance::opacity`, `SetLayeredWindowAttributes` |
| Blur | 🟡 | `DwmExtendFrameIntoClientArea` called (blur behind window) |
| Per-monitor | ⬜ | |

---

## Personalization (StartAllBack + Seelen UI)

### Desktop

| Feature | Status | Details |
|---------|--------|---------|
| Wallpaper | ✅ | Full engine with all renderers |
| Slideshow | ⬜ | |
| Multi-monitor | ✅ | Engine supports per-monitor assignment |

### Taskbar

| Feature | Status | Details |
|---------|--------|---------|
| Position | ✅ | `WinTaskbarAdapter` reads/writes `TaskbarAl` registry key |
| Transparency | 🟡 | Struct field + toggle control wired |
| Blur | ⬜ | |
| Corner radius | 🟡 | Slider control wired (0–20px) |
| Icon size | ✅ | `WinTaskbarAdapter` reads/writes `TaskbarSmallIcons` |
| Grouping | ⬜ | |
| Labels | 🟡 | Toggle control wired |
| Auto hide | ✅ | `WinTaskbarAdapter` reads/writes `TaskbarAutoHide` |
| Animation | ⬜ | |

### Start Menu

| Feature | Status | Details |
|---------|--------|---------|
| Replace | 🟡 | Toggle control wired |
| Style | ✅ | Dropdown: Win11/Win10/Classic |
| Size | ⬜ | |
| Transparency | 🟡 | Toggle control wired |
| Search | ⬜ | |
| Power menu | ⬜ | |

### Explorer

| Feature | Status | Details |
|---------|--------|---------|
| Acrylic | ✅ | Toggle control wired + WinExplorerAdapter stub |
| Mica | ✅ | Toggle control wired |
| Compact mode | 🟡 | Toggle control wired |
| Context menu | 🟡 | `showContextMenuIcons` toggle wired |
| Ribbon | ⬜ | |
| Navigation pane | 🟡 | Toggle control wired |

### System Tray

| Feature | Status | Details |
|---------|--------|---------|
| Layout | ⬜ | |
| Overflow | ⬜ | |
| Icon spacing | ⬜ | |

### Window

| Feature | Status | Details |
|---------|--------|---------|
| Rounded corners | ⬜ | |
| Shadow | ⬜ | |
| Animation | ⬜ | |
| Snap | ⬜ | |
| Transparency | ⬜ | |

### Services (Desktop Personalization Module)

| Service | Status | Details |
|---------|--------|---------|
| PersonalizationService | ✅ | Orchestrator for all managers |
| TaskbarManager | ✅ | `setAutoHide`, `setAlignment`, `refreshTaskbar` |
| ExplorerManager | ✅ | Real implementation |
| ThemeManager | ✅ | Applies visual styles |
| AppearanceManager | ✅ | Visual settings |
| EffectsManager | ✅ | Acrylic/Mica/effects |
| DesktopManager | ✅ | Desktop management |
| ProfileManager | ✅ | `listProfiles`, `load/save/delete/export/import`, `rollbackToLastGood` |
| SettingsManager | ✅ | Settings persistence |
| Platform adapter stub | 🟡 | `Win32PlatformAdapter` exists, real Win32 hooking TBD |

### Personalization Module (new)

| Component | Status | Details |
|-----------|--------|---------|
| IPersonalizationService | ✅ | `TaskbarSettings`, `ExplorerSettings`, `StartMenuSettings` structs |
| PersonalizationService | ✅ | Delegates to WinTaskbarAdapter, WinExplorerAdapter, WinStartMenuAdapter |
| PersonalizationModule | ✅ | `rex.personalization`, registers IPersonalizationService in container |
| WinTaskbarAdapter | ✅ | Reads/writes `TaskbarAl`, `TaskbarAutoHide`, `TaskbarSmallIcons` via registry |
| WinExplorerAdapter | 🟡 | Placeholder — `apply()` writes invented values (audit §5.9 #4, 2026-10-17) |
| WinStartMenuAdapter | 🟡 | Placeholder — invented keys (`Start_ShowRecommended`; `compactMode`→`DisallowShaking`) (audit §5.9 #4, 2026-10-17) |
| PersonalizationPage | ✅ | Full workspace: Toolbar (Apply/Reset), Tabs (Taskbar/Explorer/StartMenu), Toggles, Dropdowns, Slider, PropertyGrid inspector |

> ⚠️ **2026-10-17 audit §5.9 #4/#3/#9:** the Start-menu/Explorer adapters above are placeholders claiming complete (`apply()` writes invented values) — now marked 🟡. Also: two parallel personalization stacks exist and the richer native one is never constructed (factory never called), so the whole stack is compiled-but-unreferenced (§6). Not user-visible today; owner call: implement, or prune with the dead-module batch (CMakeLists-gated).

---

## Windhawk-like Features

| Feature | Status | Details |
|---------|--------|---------|
| Mod install/enable/disable/configure/update | ⬜ | |
| Explorer hooks | ⬜ | |
| Taskbar hooks | ⬜ | |
| Start Menu hooks | ⬜ | |
| DWM hooks | ⬜ | |
| Shell hooks | ⬜ | |
| Mod repository (browse/search/ratings/versioning) | ⬜ | |

---

## Marketplace

| Feature | Status | Details |
|---------|--------|---------|
| Curl client | 🟡 | `CurlMarketplaceClient.cpp` — basic HTTP client |
| Wallpaper assets | ⬜ | |
| Widget assets | ⬜ | |
| Themes | ⬜ | |
| Icons | ⬜ | |
| Fonts | ⬜ | |
| Cursors | ⬜ | |
| Plugins | ⬜ | |
| Shaders | ⬜ | |
| Scenes | ⬜ | |
| Ratings/Reviews | ⬜ | |
| Screenshots | ⬜ | |
| Changelog | ⬜ | |
| Author pages | ⬜ | |
| Version history | ⬜ | |
| Download queue/progress/resume/cancel | ⬜ | |
| Auto-update | ⬜ | |

---

## Theme System

| Feature | Status | Details |
|---------|--------|---------|
| ThemeRegistry | ✅ | `modules/UI/services/ThemeRegistry` |
| ThemeService | ✅ | `services/ThemeService` |
| TokenManager | ✅ | `services/TokenManager` |
| StyleResolver | ✅ | `services/StyleResolver` |
| ComponentRegistry | ✅ | `services/ComponentRegistry` |
| Theme editor | ⬜ | |
| Theme preview | ⬜ | |
| Theme export/import | ⬜ | |
| Theme inheritance | ⬜ | |
| Theme sharing | ⬜ | |
| Theme variables | ⬜ | |
| Theme presets | ⬜ | |

---

## Asset Manager

| Feature | Status | Details |
|---------|--------|---------|
| Fonts | ⬜ | |
| Icons | ⬜ | |
| Cursor packs | ⬜ | |
| Sounds | ⬜ | |
| Wallpapers | ⬜ | Separate from wallpaper engine |
| Theme packages | ⬜ | |

---

## Profiles

| Feature | Status | Details |
|---------|--------|---------|
| ProfileManager | ✅ | `listProfiles`, CRUD, import/export, rollback |
| Gaming profile | ⬜ | |
| Battery profile | ⬜ | |
| Work profile | ⬜ | |
| Movie profile | ⬜ | |
| Night profile | ⬜ | |
| Custom profiles | 🟡 | Generic Profile struct + manager, no predefined types |
| Auto switching | ⬜ | |

---

## Backup & Restore

| Feature | Status | Details |
|---------|--------|---------|
| Export profiles | ✅ | `ProfileManager::exportProfile` |
| Import profiles | ✅ | `ProfileManager::importProfile` |
| Rollback | ✅ | `ProfileManager::rollbackToLastGood` |
| Full backup | ⬜ | |
| Full restore | ⬜ | |
| Cloud sync | ⬜ | |

---

## Shell UI

> **RETIRED — historical snapshot.** The native C++ shell described below was archived to `dump/` (2026-07 era; the orphaned `wallpaper/renderer/` subsystem joined it there on 2026-10-19). The shipping UI is the Electron app (`app/`). Statuses in this section are **not maintained** — use `TODO.md` + `everything.md` for current state.

### Component Library (24 primitives)

| Component | Status | Details |
|-----------|--------|---------|
| Button | ✅ | `Button.hpp/.cpp` |
| Card | ✅ | `Card.hpp/.cpp` |
| SectionHeader | ✅ | `SectionHeader.hpp/.cpp` |
| Toggle | ✅ | `Toggle.hpp/.cpp` |
| Slider | ✅ | `Slider.hpp/.cpp` |
| SearchBar | ✅ | `SearchBar.hpp/.cpp` |
| Gallery | ✅ | `Gallery.hpp/.cpp` — card grid, scroll, spring animation |
| Table | ✅ | `Table.hpp/.cpp` |
| TreeView | ✅ | `TreeView.hpp/.cpp` |
| PropertyGrid | ✅ | `PropertyGrid.hpp/.cpp` — key-value inspector |
| ProgressBar | ✅ | `ProgressBar.hpp/.cpp` |
| Skeleton | ✅ | `Skeleton.hpp/.cpp` — loading placeholders |
| Badge | ✅ | `Badge.hpp/.cpp` |
| ModalDialog | ✅ | `ModalDialog.hpp/.cpp` — command palette overlay |
| AlertDialog | ✅ | `AlertDialog.hpp/.cpp` |
| ColorPicker | ✅ | `ColorPicker.hpp/.cpp` |
| Dropdown | ✅ | `Dropdown.hpp/.cpp` |
| Tooltip | ✅ | `Tooltip.hpp/.cpp` |
| ContextMenu | ✅ | `ContextMenu.hpp/.cpp` |
| Tabs | ✅ | `Tabs.hpp/.cpp` |
| Breadcrumb | ✅ | `Breadcrumb.hpp/.cpp` |
| Toolbar | ✅ | `Toolbar.hpp/.cpp` |
| StatusBar | ✅ | `StatusBar.hpp/.cpp` — left/right items, progress bar |
| SplitView | ✅ | `SplitView.hpp/.cpp` — horizontal/vertical resizable panes |

### Pages / Workspaces

| Page | Status | Details |
|------|--------|---------|
| Home | ✅ | Welcome card, active wallpaper info |
| Wallpaper | ✅ | Full workspace: Toolbar, Gallery, PropertyGrid, Sliders (brightness/volume), Dropdown (monitor), directory scanner |
| Widgets | ✅ | Full workspace: Toolbar (add/remove), Gallery (instances), PropertyGrid (details), Dropdown (type selector) |
| Personalization | ✅ | Full workspace page: 3 tabs (Taskbar/Explorer/Start Menu), controls, property inspector |
| Settings | 🟡 | `SettingsPage` exists, renders |
| Developer | 🟡 | `DeveloperPage` exists, renders |
| Marketplace | 🟡 | `MarketplacePage` exists, renders placeholder |

### Shell Infrastructure

| Feature | Status | Details |
|---------|--------|---------|
| Sidebar | ✅ | `GlowTextButton` list, 7 entries, click to switch pages |
| Title bar | ✅ | Custom title bar with drag |
| Command palette | ✅ | `Ctrl+P` → `ModalDialog`, `Escape` to close |
| Right-click menu | ✅ | `ContextMenu` with Refresh/Settings/Exit |
| Notifications | ✅ | `NotificationToast`, polling from `INotificationService`, auto-expire |
| Search bar | ✅ | Top toolbar, dispatches to `ISearchService` |
| Status bar | ✅ | Left/right items, connected to services |
| Breadcrumb | ✅ | Page navigation path |
| Tooltips | ✅ | `Tooltip` component |
| Layered window rendering | ✅ | D2D1 → WIC → DIB → `UpdateLayeredWindow` |
| Dot grid background | ✅ | Animated background |
| Intro animation | ✅ | `BlurTextTitle` on startup |
| FPS | 🟡 | Always shows "60" in inspector (hardcoded) |

---

## Dashboard

| Feature | Status | Details |
|---------|--------|---------|
| Recent activity | ⬜ | |
| Quick actions | ⬜ | |
| Favorites | ⬜ | |
| Resource usage | ⬜ | |
| Running modules | ⬜ | |
| News | ⬜ | |
| Updates | ⬜ | |
| Shortcuts | ⬜ | |

---

## Performance

| Feature | Status | Details |
|---------|--------|---------|
| CPU monitoring | ⬜ | |
| GPU monitoring | ⬜ | |
| RAM monitoring | ⬜ | |
| VRAM monitoring | ⬜ | |
| Temperature | ⬜ | |
| FPS counter | 🟡 | Hardcoded "60" in shell inspector |
| Frame time | ⬜ | |
| Module usage | ⬜ | |
| Process monitor | ⬜ | |

---

## Automation

| Feature | Status | Details |
|---------|--------|---------|
| Schedule wallpaper | ⬜ | |
| Schedule themes | ⬜ | |
| Trigger profiles | ⬜ | |
| Battery rules | ⬜ | |
| Time rules | ⬜ | |
| Game detection | ⬜ | |

---

## Plugin SDK

| Feature | Status | Details |
|---------|--------|---------|
| APIs | 🟡 | `IModule`, `IModuleContext`, `ServiceContainer` |
| Lifecycle | ✅ | `initialize/start/stop/shutdown`, `LifecycleCoordinator` |
| Events | ✅ | `EventBus`, `ModuleEvent` |
| Packaging | ⬜ | |
| Publishing | ⬜ | |

---

## Package Manager

| Feature | Status | Details |
|---------|--------|---------|
| Install | ⬜ | |
| Remove | ⬜ | |
| Dependencies | ✅ | `DependencyResolver` exists for modules |
| Versioning | ⬜ | |
| Rollback | ⬜ | |

---

## Cross-cutting Systems

| System | Status | Details |
|--------|--------|---------|
| ModuleRegistry | ✅ | Register/unregister/get by ID/state |
| ModuleManager | ✅ | Load/unload/start/stop/enable/disable |
| ModuleLoader | ✅ | Built-in factory + dynamic DLL loading |
| ModuleOrchestrator | ✅ | Initialization, dependency resolution, startup order, health monitoring |
| LifecycleCoordinator | ✅ | Dependency graph, initialize/start/stop/shutdown all in order |
| HealthMonitor | ✅ | Crash count, restart count, per-module health |
| ConfigurationStore | ✅ | JSON-based `config/modules.json` |
| PluginSandbox | ✅ | `InProcessSandbox` for dynamic modules |
| SignatureVerifier | ✅ | Module signature verification |
| UpdateManager | 🟡 | Stub interface |
| DependencyResolver | ✅ | Topological sort with cycle detection |
| ServiceContainer | ✅ | Type-indexed service registry |
| EventBus | ✅ | Publish/subscribe event system |
| ModuleContext | ✅ | Config path, events, service container access |
| Permission manager | ⬜ | |
| Download manager | ⬜ | |
| Resource cache | 🟡 | `TextureCache` in graphics |
| Thumbnail service | ⬜ | |
| Package installer | ⬜ | |
| Analytics/telemetry | ⬜ | |
| Crash reporting | ⬜ | |
| Recovery mode | ⬜ | |
| Safe mode | ⬜ | |
| Undo/Redo manager | ⬜ | |
| Clipboard manager | ⬜ | |
| History manager | ⬜ | |
| Background task scheduler | ⬜ | |

---

## Developer Tools

| Feature | Status | Details |
|---------|--------|---------|
| Module inspector | ⬜ | |
| Event viewer | ⬜ | |
| Logger | 🟡 | `spdlog` linked, basic logging |
| FPS overlay | ⬜ | |
| GPU profiler | ⬜ | |
| Memory profiler | ⬜ | |
| Theme inspector | ⬜ | |
| Search inspector | ⬜ | |
| Renderer inspector | ⬜ | |

---

## Priority 3 (Future)

| Feature | Status |
|---------|--------|
| Wallpaper Editor (timeline/layers/audio/effects/preview/export) | ⬜ |
| Theme Editor (visual editor, live preview, publish) | ⬜ |
| Widget Designer (visual editor, property editor, preview, packaging) | ⬜ |
| AI wallpaper generation | ⬜ |
| AI theme creation | ⬜ |
| AI automation | ⬜ |
| AI recommendations | ⬜ |

---

## Legend

| Icon | Meaning |
|------|---------|
| ✅ | Implemented and functional |
| 🟡 | Partial / stub / skeleton only |
| ⬜ | Not started |

---

*Update this file by changing the status icon (⬜ → 🟡 → ✅) and adding file paths when a feature ships.*
