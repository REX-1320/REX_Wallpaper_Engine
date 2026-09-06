# REX CURRENT STATE REPORT

## 1. REPOSITORY STRUCTURE

```
D:\REX_doc\                              (monorepo root)
├── app/                                 Electron + React frontend
│   ├── electron/                        Main process (11 files)
│   │   ├── main.cjs                     179 lines — entry point
│   │   ├── preload.cjs                  24 lines — context bridge
│   │   ├── wallpaperRuntime.cjs         155 lines — native runtime client
│   │   ├── app-state-manager.cjs        195 lines — UNTRACKED, DEAD
│   │   ├── tray-manager.cjs             111 lines — UNTRACKED, DEAD
│   │   ├── wallpaper-library.cjs        181 lines — UNTRACKED, DEAD
│   │   └── supervisor/                  5 files — UNTRACKED, DEAD
│   ├── src/                             React frontend (~100+ files)
│   │   ├── App.jsx                      Tab router
│   │   ├── wallpapers/                  Gallery, preview, inspector, playlist
│   │   ├── components/                  61 ShadCN UI + 14 property editors
│   │   ├── widgets/                     Widget workspace + inspector
│   │   ├── hooks/                       useAppState.jsx (ORPHANED)
│   │   └── lib/                         utils.js
│   ├── assets/                          UNTRACKED: rex-tray.ico
│   ├── dist/                            Vite build output
│   └── dist-exe/                        Packaged REX.exe
├── rex-core/                            C++ engine (~160+ source files)
│   ├── include/rex/wallpaper/           Public interfaces
│   ├── src/wallpaper/                   Engine, decoder, video, renderer, playlist
│   ├── src/graphics/                    D3D12 renderer, compositor, effects
│   ├── src/platform/windows/            WorkerW, Progman, EDID, desktop host
│   ├── src/modules/                     7 module subsystems (~170+ files)
│   ├── src/desktop_personalization/     Theme, effects, explorer, taskbar
│   ├── tests/                           29 test files, ~280+ test cases
│   └── tools/                           GenTestMedia, rex-minidump, PreviewClient
├── rex-shared/                          Shared C++ lib (Logger, UUID, Stopwatch, Result)
├── rex-shell/                           C# WPF shell (IPC client, widget host, tray)
├── docs/                                RES specs + developer guides
├── proto/                               Protobuf schemas
├── src/ui/surface/                      TypeScript surface rendering framework
└── bench-media/                         UNTRACKED: test videos, screenshots, logs
```

## 2. BUILD / TOOLCHAIN

| Component | Detail |
|-----------|--------|
| Node | v24.18.0 |
| Package manager | pnpm |
| Electron | 43.2.0 |
| React | 19.2.8 |
| Vite | 8.2.0 |
| C++ standard | C++20 |
| Compiler | MSVC (VS2022) |
| CMake | 3.20+ |
| vcpkg deps | fmt, spdlog, gtest, ffmpeg (avcodec, avformat, swscale) |
| Windows APIs | D3D11, D3D12, DXGI, DirectComposition, WIC, DWM, WASAPI, COM |
| Packaging | electron-builder 26.15.3, --win portable |
| Linter | oxlint 1.76.0 |

### Build Commands

```
# Frontend
pnpm run build          → vite build
pnpm run lint           → oxlint
pnpm run electron       → vite build && electron .
pnpm run dist:win       → vite build && node scripts/package-win.cjs

# C++ native
cmake -B .build -G "Visual Studio 17 2022" -A x64
cmake --build .build --config Release

# Packaging
npx electron-builder --win portable --config.win.signAndEditExecutable=false
```

### ExtraResources (bundled in packaged exe)

| From | To |
|------|----|
| `rex-core/.build/Release/rex-wallpaper-cli.exe` | `bin/rex-wallpaper-cli.exe` |
| `rex-core/.build/Release/spdlog.dll` | `bin/spdlog.dll` |
| `rex-core/.build/Release/fmt.dll` | `bin/fmt.dll` |

**NOTE:** `package.json` extraResources references `rex-wallpaper-cli.exe` but `getWallpaperCliPath()` in HEAD `main.cjs` also points to `rex-wallpaper-cli.exe`. The untracked `RuntimeSettings`/`PlaylistManager` work references `rex-wallpaper-runtime.exe` (a different binary name).

## 3. CURRENT APPLICATION ARCHITECTURE

```
REX.exe (Electron 43.2.0)
├── main process (main.cjs)
│   ├── WallpaperRuntime → spawns → rex-wallpaper-cli.exe --runtime
│   │   └── [length-prefixed JSON on stdin/stdout]
│   ├── 5 IPC handlers (wallpaper:set, wallpaper:video, wallpaper:update,
│   │                    wallpaper:capabilities, shell:show-in-folder)
│   ├── Runtime event forwarding → renderer
│   └── will-quit → runtime.stop()
├── renderer process (React SPA)
│   ├── preload.cjs → window.rexDesktop bridge (9 APIs)
│   ├── WallpaperGallery → file import, grid, sort, playlist
│   ├── WallpaperPreview → CSS-filtered preview canvas
│   ├── WallpaperMedia → <img>/<video> + normalized audio
│   ├── MediaControlsOverlay → full media player controls
│   ├── DesktopApply → bridges React state → IPC
│   ├── Inspector → 3 of 14 sections mounted
│   └── WidgetsWorkspace → widget gallery + inspector
├── GPU process
└── Utility process

rex-wallpaper-cli.exe --runtime (native C++ process)
├── Engine (IWallpaperEngine)
│   ├── D3D12Renderer (device, swap chain, HLSL shaders)
│   ├── VideoManager
│   │   ├── FfmpegVideoDecoder (D3D11VA HW accel)
│   │   │   └── 8-slot shared texture pool + D3D11 VideoProcessor
│   │   ├── VideoPlaybackController (decode thread + SPSC FrameScheduler)
│   │   ├── AudioPlayer (FFmpeg audio + WASAPI)
│   │   └── MonitorCompositor (per-monitor HWND + D3D12Renderer)
│   ├── IDesktopHost (WorkerW/Progman)
│   └── SettingsService (per-monitor persistence)
├── Crash diagnostics (VEH, hang watchdog, thread dump)
└── Telemetry (GET_STATUS response, --benchmark mode)
```

### Process Tree

```
REX.exe
├── main (Node.js event loop)
├── renderer (Chromium tab)
├── GPU (D3D12/DXGI)
├── Utility (network/audio)
└── rex-wallpaper-cli.exe --runtime (native C++ wallpaper engine)
```

## 4. ELECTRON

### main.cjs (179 lines)

**Active imports:** Only `wallpaperRuntime.cjs`

**IPC handlers (5):**

| Channel | Handler | Preload | Renderer | Status |
|---------|---------|---------|----------|--------|
| `wallpaper:set` | main.cjs:96 | `setDesktopWallpaper` | `desktop.js:47` | ✅ WIRED |
| `wallpaper:video` | main.cjs:112 | `setDesktopVideo` | `desktop.js:40,46,54,67` | ✅ WIRED |
| `wallpaper:update` | main.cjs:148 | `updateWallpaper` | `desktop.js:75,91` | ✅ WIRED |
| `wallpaper:capabilities` | main.cjs:158 | `getCapabilities` | **NONE** | ⚠️ EXPOSED, UNUSED |
| `shell:show-in-folder` | main.cjs:162 | `showInFolder` | `WallpaperGallery.jsx:129` | ✅ WIRED |

**Push channels:**

| Channel | Sender | Preload | Renderer | Status |
|---------|--------|---------|----------|--------|
| `wallpaper:event` | main.cjs:92 | `onWallpaperEvent` | `runtimeEvents.js` | ✅ WIRED |
| `navigate` | tray-manager.cjs:92 | **NONE** | **NONE** | ❌ DEAD (tray not loaded) |

### preload.cjs (24 lines)

Exposes `window.rexDesktop` with 9 APIs. Missing channels that renderer tries to use:

| Missing API | Renderer Caller | Impact |
|-------------|-----------------|--------|
| `appState.get/set` | `useAppState.jsx:11,25` | Silent fallback to default |
| `saveThumbnail` | `thumbnail.js:15` | Silent no-op |
| `deleteThumbnail` | `thumbnail.js:20` | Silent no-op |

### wallpaperRuntime.cjs (155 lines)

- **Protocol:** Length-prefixed UTF-8 JSON on stdin/stdout
- **Constructor:** `(cliPath, logLine)` — NOTE: HEAD version, different from untracked version which takes `(cliPath, { dataDir, logLine })`
- **Commands sent:** INIT, LOAD, PLAY, PAUSE, STOP, SHUTDOWN, GET_CAPABILITIES, SET_*, GET_STATUS
- **Events received:** READY, LOADED, PLAYING, PAUSED, STOPPED, UNLOADED, ERROR

### Dead Code in electron/

| File | Lines | Status |
|------|-------|--------|
| `app-state-manager.cjs` | 195 | NOT imported by main.cjs |
| `tray-manager.cjs` | 111 | NOT imported by main.cjs |
| `wallpaper-library.cjs` | 181 | NOT imported by main.cjs |
| `supervisor/supervisor.cjs` | 235 | NOT imported by main.cjs |
| `supervisor/runtime-manager.cjs` | 80 | Only by supervisor |
| `supervisor/state-store.cjs` | 74 | Only by supervisor |
| `supervisor/crash-reporter.cjs` | 109 | Only by supervisor |
| `supervisor/minidump.cjs` | 80 | Only by supervisor |

**8 of 11 electron/ files are dead code.**

### Missing from main.cjs (compared to preload expectations)

- No `appState.*` IPC handlers
- No `gallery.*` IPC handlers
- No `window:minimize/toggle-maximize/close/is-maximized` handlers
- No `app:quit` handler
- No `renderer-ping` handler
- No `wallpaper:pause/resume` handlers
- No `wallpaper:save-thumbnail/delete-thumbnail` handlers
- No window state persistence
- No tray icon
- No supervisor/watchdog
- No crash recovery

## 5. REACT / UI

### App Root

`App.jsx` (72 lines) — tab-based router with 4 active tabs: Home, shadcn, Widgets, Wallpapers

### Active Tabs

| Tab | Component | Status |
|-----|-----------|--------|
| Home | Greeting + SpecularButton | ✅ Working |
| Widgets | WidgetsWorkspace | ✅ Working (gallery + inspector) |
| Wallpapers | WallpaperGallery + WallpaperPreview + Inspector | ✅ Working |
| shadcn | ShadcnGallery | ✅ Working (component showcase) |

### 15 Dead Nav Items

SidebarLayout defines 20 nav items. App.jsx only handles 4 labels. The other 15 render `null`: Buttons, Settings, Profile, Alerts, Themes, Personalization, Marketplace, Downloads, Collections, Performance, Modules, Activity, Settings (dup), Developer, About.

### Wallpaper Module

| Component | Lines | Status |
|-----------|-------|--------|
| `WallpaperGallery.jsx` | 663 | ✅ File import, grid, sort, context menu, playlists |
| `WallpaperPreview.jsx` | 44 | ✅ Preview canvas with CSS filters |
| `WallpaperVisual.jsx` | 45 | ✅ Per-wallpaper CSS filter chain |
| `WallpaperMedia.jsx` | 164 | ✅ `<img>`/`<video>` with normalized audio |
| `MediaControlsOverlay.jsx` | 1050 | ✅ Full media player overlay |
| `DesktopApply.jsx` | 62 | ✅ Bridges React state → IPC |
| `desktop.js` | 97 | ✅ IPC functions |
| `store.js` | 164 | ✅ ExternalStore for wallpapers/playlists |
| `registry.js` | 358 | ✅ 30+ file type detection, defaults |
| `runtimeEvents.js` | 35 | ✅ Runtime event bridge |
| `thumbnail.js` | 94 | ⚠️ EXISTS but NOT imported (bridge missing) |
| `mediaBridge.js` | 17 | ✅ In-memory DOM element registry |
| `GalleryGrid.jsx` | 73 | ❌ Dead — windowed grid, never imported |

### Inspector (3 of 14 sections mounted)

`Inspector.jsx` imports only:

| Section | Status |
|---------|--------|
| AppearanceSection | ✅ Mounted |
| RendererSection | ✅ Mounted |
| ActionsSection | ✅ Mounted (7 of 9 buttons are noop stubs) |
| DisplaySection | ✅ Mounted in WallpaperGallery, NOT in Inspector |
| PlaybackSection | ❌ NOT imported |
| MonitorSection | ❌ NOT imported |
| AudioSection | ❌ NOT imported |
| GeneralSection | ❌ NOT imported |
| ScheduleSection | ❌ NOT imported |
| InfoSection | ❌ NOT imported |
| InteractionSection | ❌ NOT imported |
| PerformanceSection | ❌ NOT imported |
| PlaylistSection | ❌ NOT imported |
| AdvancedSection | ❌ NOT imported |

### Playlist Engine

`engine.js` (1046 lines) — fully implemented with 23 change modes, 15 playback orders, 25 transition types, preset management, system monitoring (CPU/GPU/RAM/battery). `PlaylistEngineDialog.jsx` (471 lines) — full settings dialog, actively used.

## 6. WALLPAPER SYSTEM

### Type Implementation Status

| Type | Detection | Gallery | Apply Path | Native Renderer | Status |
|------|-----------|---------|------------|-----------------|--------|
| IMAGE | ✅ `registry.js` | ✅ Grid tile | ✅ `desktop.js` → `wallpaper:set` → `SetImageWallpaper()` (OS API) | `ImageRenderer` (stub, compositor only) | ✅ FULLY WIRED |
| VIDEO | ✅ `registry.js` | ✅ Grid tile + `<video>` preview | ✅ `desktop.js` → `wallpaper:video` → Engine LOAD+PLAY | D3D12Renderer + FfmpegVideoDecoder | ✅ FULLY WIRED |
| SHADER | ✅ `registry.js` | ✅ Grid tile | ❌ No apply path | `ShaderRenderer` (own D3D12, HLSL) | ⚠️ RENDERER EXISTS, NO APPLY PATH |
| WEB | ❌ Not detected | ❌ | ❌ | ❌ | ❌ NOT IMPLEMENTED |
| SCENE | ❌ Not detected | ❌ | ❌ | ❌ | ❌ NOT IMPLEMENTED |
| AUDIO VIS | ❌ Not detected | ❌ | ❌ | `AudioVisualizerRenderer` (WASAPI+FFT) | ⚠️ RENDERER EXISTS, NO DETECTION/APPLY |

### Full Video Pipeline Trace

```
Gallery: file drop → detectFileType("video") → createWallpaperFromType("VIDEO")
  → store.set(id, wallpaper)
  → DesktopApply.jsx watches loadKey
  → applyWallpaperToDesktop(w) [desktop.js:40]
  → window.rexDesktop.setDesktopVideo({ path, mode, fps, loop, muted, volume, speed })
  → preload.cjs → ipcRenderer.invoke('wallpaper:video', options)
  → main.cjs: ipcMain.handle('wallpaper:video')
    → runtime.init(fps, true)
    → runtime.send('LOAD', { path, type:'video', mode, loop, muted, volume, speed })
    → runtime.send('PLAY')
  → rex-wallpaper-cli.exe --runtime receives JSON frames
    → INIT → CreateWallpaperEngine → D3D12Renderer + VideoManager
    → LOAD → VideoManager.OpenVideo → VideoPlaybackController.Open
      → FfmpegVideoDecoder.Open (D3D11VA hardware decode)
      → 8-slot shared texture pool (D3D11 + NT handles)
    → PLAY → DecoderThread → DecodeFrame → ConvertAndSignal → FrameScheduler.Push
    → RenderLoop → FrameScheduler.Tick → D3D12Renderer.DrawVideoFrame → Present
```

## 7. WALLPAPER GALLERY

| Feature | Status |
|---------|--------|
| File import (drag/drop/click) | ✅ |
| Type detection (30+ extensions) | ✅ |
| Grid display with thumbnails | ✅ |
| Sort (name, type, size, date) | ✅ |
| Context menu (add to playlist, rename, remove, open folder) | ✅ |
| Playlist assignment | ✅ |
| Playlist engine (23 modes, 15 orders, 25 transitions) | ✅ |
| Preview with CSS filters | ✅ |
| Media controls overlay | ✅ |
| Thumbnail generation (canvas WebP) | ⚠️ Code exists in `thumbnail.js` but NOT imported |
| Thumbnail persistence | ❌ `saveThumbnail` bridge missing in preload |
| Gallery persistence via AppStateManager | ❌ `appState` bridge missing in preload |
| Missing-file detection | ❌ |
| Virtualization (GalleryGrid) | ⚠️ Code exists but NOT imported |
| Favorites | ❌ Inspector sections not mounted |
| Search filtering | ⚠️ Search pill exists, no filtering logic connected |

Original wallpaper files are **referenced by path** (not copied).

## 8. WALLPAPER RUNTIME (NATIVE)

### Entry Point

`wallpaper_cli_main.cpp` (2159 lines) — `wmain()`

### Three Modes

1. **CLI** (`--file`): Load and play immediately, Win32 message pump
2. **Runtime** (`--runtime`): Persistent process, JSON-over-stdin/stdout
3. **Benchmark** (`--benchmark`): Standalone D3D11VA→D3D12 pipeline test

### Runtime Protocol

- **Transport:** `[uint32 LE length][UTF-8 JSON]` on stdin/stdout
- **21 commands:** INIT, LOAD, PLAY, PAUSE, STOP, UNLOAD, SET_FILL_MODE, SET_VOLUME, SET_SPEED, SET_LOOP, SET_BRIGHTNESS, SET_CONTRAST, SET_SATURATION, SET_MONITOR, SET_CLIENT_PID, GET_FRAME_SURFACE, GET_STATUS, GET_CAPABILITIES, SHUTDOWN
- **8 events:** READY, LOADED, PLAYING, PAUSED, STOPPED, UNLOADED, ERROR, FRAME_READY (~30Hz)

### Crash Diagnostics

- VEH (CrashHandler): logs to `%TEMP%\REX\crash.log`
- UnhandledExceptionFilter: dumps all thread stacks
- HangWatchdog: T+10s first dump, then every 5s
- SIGABRT/terminate handlers

## 9. VIDEO PIPELINE

| Stage | Implementation | Detail |
|-------|---------------|--------|
| Demux | FFmpeg (libavformat) | Automatic stream selection by resolution |
| Codec detection | FFmpeg | H.264, HEVC, AV1 (via FFmpeg codecs) |
| HW decode | D3D11VA | `av_hwdevice_ctx_alloc(D3D11VA)`, `avcodec_open2` |
| SW fallback | swscale | BGRA/RGBA64 via `sws_getContext` |
| Frame pool | 8 slots | D3D11 textures, `SHARED \| SHARED_NTHANDLE` |
| GPU conversion | D3D11 VideoProcessor | NV12→BGRA (SDR) or RGBA16F (HDR) |
| Cross-device | Shared NT handles + fence | D3D11 signals fence → D3D12 waits before draw |
| Frame scheduling | SPSC lock-free ring | Latest-frame-wins, intermediate dropped |
| Color space | DXGI_COLOR_SPACE | BT.601/709/2020 × SDR/PQ/HLG |
| HDR | PQ/HLG → tone map | ACES-fit for HDR→SDR, PQ-encode for HDR output |
| Render | D3D12Renderer | HLSL PS (color pipeline: decode→adjust→tonemap→encode) |
| Present | DXGI SwapChain | FLIP_DISCARD, sync interval 1 (vsync), frame latency waitable |

**CPU copies:** Software fallback path (sws_scale → staging texture → CopyResource)
**GPU copies:** Hardware path is zero-copy (D3D11VP → shared handle → D3D12 open)
**Per-frame allocations:** Software path creates temp command allocator + list per upload

## 10. PERFORMANCE / TELEMETRY

### Currently Measured (22 fields via GET_STATUS)

| Field | Source |
|-------|--------|
| `framesDecoded` | VideoPlaybackController atomics |
| `framesConsumed` | VideoPlaybackController atomics |
| `framesDropped` | computed |
| `decoderFps` | frame count / elapsed |
| `renderFps` | consumer count / elapsed |
| `videoWidth`, `videoHeight` | VideoManager |
| `ringSlotCount`, `ringBytes` | FrameScheduler |
| `textureBytes`, `uploadBytes` | D3D12Renderer |
| `textureRecreations`, `uploads` | D3D12Renderer |
| `allocatorsPerFrame` | D3D12Renderer |
| `scenesLoaded`, `scenesReleased` | Engine atomics |
| `decodeWidth`, `decodeHeight` | adaptive resolution |
| `hdrActive`, `displayMaxNits` | SwapChain |
| `privateBytesMB` | K32GetProcessMemoryInfo |

### NOT Currently Measured

- GPU dedicated memory usage
- GPU budget
- Per-frame pipeline timing (decode→share→render→present)
- Frame drops by category (decode vs render vs display)
- Cross-adapter latency
- D3D11→D3D12 fence wait time
- WASAPI buffer underruns
- Explorer restart count
- Monitor hot-plug events

## 11. CROSS-HARDWARE

| Concern | Status |
|---------|--------|
| D3D12 adapter selection | `Device::Initialize()` — high-performance preference, fallback to default |
| D3D11 adapter for decode | Created on same adapter via DXGI LUID match |
| Cross-adapter detection | `GetAdapterLuid()` logged in VideoManager |
| Shared handles | NT handles require same physical adapter — enforced by LUID matching |
| AMD vs NVIDIA | No adapter-specific code paths |
| Hybrid graphics | Not explicitly handled — depends on DXGI enumeration order |

**Guarantee:** Decoder GPU = Renderer GPU = Display GPU (enforced by same-adapter creation).

## 12. WORKERW / DESKTOP INTEGRATION

| Component | Status |
|-----------|--------|
| Progman acquisition | ✅ `ProgmanHost.cpp` — `FindWindow("Progman")`, `SendMessage(WM_SPAWNWORKERW)` |
| WorkerW creation | ✅ `WorkerWManager.cpp` — finds blank WorkerW or creates synthetic |
| Render window parenting | ✅ `CreateRenderWindow()` — reparents to WorkerW |
| Monitor thread | ✅ Watches Explorer process, reacquires on failure |
| EDID extraction | ✅ `MonitorEdid.cpp` — PnP registry, FNV-1a hash |
| Multi-monitor | ✅ `MultiMonitorDesktopHost.cpp` — per-monitor HWND |
| Explorer restart | ✅ `verify-shell-restart.ps1` tests this |
| DPI handling | ✅ Per-monitor DPI scaling |
| Z-order | ✅ `HWND_TOP` positioning |

## 13. MONITOR SYSTEM

| Feature | Status |
|---------|--------|
| Enumeration | ✅ `EnumDisplayMonitors` callback |
| EDID hash | ✅ FNV-1a from PnP registry |
| Per-monitor assignment | ✅ Via `SetPerMonitorWallpaper()` + settings persistence |
| Reconnect detection | ✅ `CheckDisplayChanges()` every 120 frames |
| Hot-plug | ✅ Adds/removes compositors dynamically |
| Persistence key | EDID hash or device name |
| Survives reboot | ✅ (via SettingsService persistence) |

## 14. PERSISTENCE

| File | Location | Writer | Atomic | Backup | Status |
|------|----------|--------|--------|--------|--------|
| `app-state.json` | `<userData>` | **NOT WIRED** | ✅ (in code) | ✅ 14-day daily (in code) | ❌ DEAD — main.cjs doesn't import AppStateManager |
| `wallpaper-library.json` | `<userData>` | **NOT WIRED** | ✅ (in code) | ❌ | ❌ DEAD — main.cjs doesn't import WallpaperLibrary |
| `supervisor-state.json` | `<userData>` | **NOT WIRED** | ✅ (in code) | ❌ | ❌ DEAD — main.cjs doesn't import Supervisor |
| `settings.json` | runtime data dir | C++ SettingsService | ✅ (tmp+rename) | ✅ | ✅ ACTIVE |
| localStorage | Browser | React components | N/A | ❌ | ✅ ACTIVE (playlist state) |

Daily backups, 14-day retention, corrupt JSON restore — all implemented in `app-state-manager.cjs` but not wired.

## 15. TRAY / BACKGROUND APP

| Feature | Status |
|---------|--------|
| Tray icon | ❌ NOT WIRED — TrayManager exists but not imported |
| Tray menu | ❌ NOT WIRED |
| Close-to-tray | ❌ NOT WIRED |
| Startup-to-tray | ❌ NOT WIRED |
| Single-instance | ❌ NOT IMPLEMENTED |
| Runtime ownership | ⚠️ Runtime survives window close (will-quit stops it) |

## 16. CRASH / HANG / WATCHDOG

| Feature | Status |
|---------|--------|
| Native VEH crash handler | ✅ `wallpaper_cli_main.cpp:285-320` |
| UnhandledExceptionFilter | ✅ `wallpaper_cli_main.cpp:322-340` |
| Hang watchdog (native) | ✅ `wallpaper_cli_main.cpp:350-381` — T+10s, then 5s |
| Thread stack dump | ✅ Toolhelp32 + StackWalk64 |
| Minidump capture | ⚠️ `rex-minidump.exe` exists, `minidump.cjs` exists, NOT WIRED |
| Crash report collection | ⚠️ `crash-reporter.cjs` exists, NOT WIRED |
| Runtime heartbeat | ⚠️ `supervisor.cjs` exists with 2s PING, NOT WIRED |
| Renderer watchdog | ⚠️ `supervisor.cjs` exists with 5s/15s stale, NOT WIRED |
| Recovery manager | ⚠️ `supervisor.cjs` exists with circuit breaker, NOT WIRED |
| Electron crash handler | ❌ NOT IMPLEMENTED |

## 17. SHADER / WEB / SCENE / AUDIO

| Type | Gallery → IPC → Runtime → Renderer → Desktop |
|------|-----------------------------------------------|
| IMAGE | ✅ Full path works |
| VIDEO | ✅ Full path works |
| SHADER | ⚠️ `ShaderRenderer` fully implemented (331 lines, own D3D12, HLSL compile, shared texture). No gallery detection, no apply path, no IPC. Dead code. |
| WEB | ❌ No code exists |
| SCENE | ❌ No code exists |
| AUDIO VIS | ⚠️ `AudioVisualizerRenderer` fully implemented (633 lines, D3D12 + WASAPI capture + FFT + 3 modes). No gallery detection, no apply path, no IPC. Dead code. |

## 18. PERSONALIZATION / THEMES / MODS

| Feature | Status |
|---------|--------|
| Theme system (C++) | ⚠️ `ThemeManager`, `AppearanceManager`, `EffectsManager` exist in `desktop_personalization/` — compiled into rex-core but not exposed to Electron |
| Theme package format | ⚠️ `ZipExtractor`, `ThemeSchema`, `ThemeTypes` exist |
| Inspector UI | ❌ "Themes" and "Personalization" nav items render null |
| Taskbar customization | ⚠️ `TaskbarManager` exists in C++ |
| Explorer integration | ⚠️ `ExplorerManager` exists in C++ |
| Mod system | ❌ Not implemented |
| Shell customization | ❌ Not implemented |

## 19. FILES FROM LOST WORK

### Surviving Untracked Files (29 files total)

**Electron (8 files) — ALL DEAD CODE:**

| File | What | Wired? | Effort |
|------|------|--------|--------|
| `app-state-manager.cjs` | State persistence with backup/recovery | ❌ | Moderate |
| `tray-manager.cjs` | System tray icon + menu | ❌ | Trivial |
| `wallpaper-library.cjs` | Gallery CRUD + IPC | ❌ | Trivial |
| `supervisor/supervisor.cjs` | Watchdog + recovery | ❌ | Significant |
| `supervisor/runtime-manager.cjs` | Runtime wrapper with ping | ❌ (by supervisor) | Trivial |
| `supervisor/state-store.cjs` | Recovery state persistence | ❌ (by supervisor) | Trivial |
| `supervisor/crash-reporter.cjs` | Crash report collection | ❌ (by supervisor) | Trivial |
| `supervisor/minidump.cjs` | Minidump capture | ❌ (by supervisor) | Trivial |

**C++ (2 files) — INTEGRATED:**

| File | What | Wired? |
|------|------|--------|
| `RuntimeSettings.hpp` | Lightweight JSON settings store | ✅ Used by PlaylistManager |
| `RuntimeSettings.cpp` | Implementation | ✅ |

**React (11 files) — MIXED:**

| File | What | Wired? | Effort |
|------|------|--------|--------|
| `useAppState.jsx` | Persisted state hook | ❌ (bridge missing) | Moderate |
| `runtimeEvents.js` | Runtime event subscription | ✅ | None |
| `thumbnail.js` | Thumbnail generation + persistence | ❌ (bridge missing) | Moderate |
| `desktop.js` | Desktop IPC functions | ✅ | None |
| `mediaBridge.js` | DOM element registry | ✅ | None |
| `GalleryGrid.jsx` | Windowed grid | ❌ | Trivial |
| `MediaControlsOverlay.jsx` | Media player overlay | ✅ | None |
| `PlaylistEngineDialog.jsx` | Playlist settings dialog | ✅ | None |
| `DisplaySection.jsx` | Display settings (monitor, mode) | ✅ (in gallery) | None |
| 10 inspector sections | Playback, Audio, Monitor, etc. | ❌ | Trivial (1 import each) |

**PowerShell scripts (23 files):**

Test/verify scripts for: tray, supervisor recovery, runtime recovery, smoke, stress, navigation, titlebar, packaged builds, shell restart, cadence.

## 20. TESTS

### C++ Test Suite (29 files, ~280+ cases)

| Test Suite | Files | Cases | Status |
|------------|-------|-------|--------|
| Core (sandbox, IPC, lifecycle, dependency) | 4 | ~14 | ✅ Complete |
| Compositor | 2 | ~7 | ✅ Complete |
| Desktop Host | 2 | ~18 | ✅ Complete |
| Settings | 5 | ~66 | ✅ Very thorough |
| Wallpaper Engine | 6 | ~30 | ✅ Very thorough |
| Commands | 2 | ~64 | ✅ Very thorough |
| Notifications | 3 | ~92 | ✅ Very thorough |
| Search | 1 | ~80 | ✅ Extremely thorough |
| Personalization | 1 | ~15 | ✅ Complete |

All tests use Google Test. Build: `cmake --build .build --config Release --target rex-core-tests` etc.

### PowerShell Verification Scripts

`verify-tray.ps1`, `verify-supervisor-recovery.ps1`, `verify-runtime-recovery.ps1`, `verify-shell-restart.ps1`, etc.

## 21. KNOWN BUGS

| Bug | Cause | Status | Severity |
|-----|-------|--------|----------|
| Head main.cjs only has 5 IPC handlers | Preload exposes 9 APIs, 4 have no handler | CONFIRMED | High |
| 8 electron modules are dead code | main.cjs doesn't import them | CONFIRMED | High |
| 11 inspector sections not mounted | Inspector.jsx imports only 3 | CONFIRMED | Medium |
| `wallpaper:capabilities` exposed but never called | No renderer code uses it | CONFIRMED | Low |
| `gallery.*` IPC handlers exist but no preload bridge | renderer can't reach them | CONFIRMED | Medium |
| `appState` bridge missing in preload | useAppState.jsx silently falls back | CONFIRMED | High |
| `thumbnail` bridge missing in preload | Thumbnail persistence silently skipped | CONFIRMED | Medium |
| ActionsSection 7/9 buttons are noop | Stub implementations | CONFIRMED | Low |
| 15 of 20 nav items render null | No content for most sidebar items | CONFIRMED | Low |

## 22. LOST FEATURES

Based on evidence in the current repository:

| Feature | Surviving Code | Missing Integration | Scope |
|---------|---------------|---------------------|-------|
| App state persistence | `app-state-manager.cjs` (195 lines) | main.cjs import + preload bridge | Moderate |
| System tray | `tray-manager.cjs` (111 lines) | main.cjs import + icon asset exists | Trivial |
| Gallery persistence | `wallpaper-library.cjs` (181 lines) | main.cjs import | Trivial |
| Crash recovery/supervisor | `supervisor/` (5 files, 579 lines) | main.cjs wiring + preload events | Significant |
| Thumbnail caching | `thumbnail.js` (94 lines) | preload bridge | Moderate |
| Persisted UI preferences | `useAppState.jsx` (31 lines) | preload bridge + consumers | Moderate |
| Full inspector (14 sections) | 10 section files (463 lines) | Inspector.jsx imports | Trivial |
| Virtualized gallery grid | `GalleryGrid.jsx` (73 lines) | WallpaperGallery.jsx import | Trivial |
| Window state persistence | AppStateManager has `window.*` schema | main.cjs window event handlers | Moderate |
| Close-to-tray | TrayManager + AppStateManager have schema | main.cjs close handler | Moderate |
| V8 memory cap | Not in HEAD main.cjs | `--js-flags --max-old-space-size=128` | Trivial |
| GPU switches | Not in HEAD main.cjs | `REX_PREFER_IGPU` gated | Trivial |
| Native RuntimeSettings | `RuntimeSettings.cpp/.hpp` (243 lines) | CMakeLists.txt source list | Trivial |
| PlaylistManager (C++) | `PlaylistManager.cpp/.hpp` (244 lines) | Already in CMakeLists.txt | None |

## 23. CURRENT STATE SUMMARY

### A. Working Now
- Vite build + Electron packaging
- Image wallpaper apply (via OS API)
- Video wallpaper apply (full D3D11VA→D3D12 pipeline)
- Gallery file import, grid display, sort, context menu
- Preview canvas with CSS filters
- Media controls overlay (seek, volume, speed, screenshot)
- Playlist engine (23 modes, 15 orders, 25 transitions)
- Runtime event bridge (READY/LOADED/PLAYING/ERROR)
- Desktop apply bridge (DesktopApply.jsx → IPC)
- Runtime telemetry (22 fields via GET_STATUS)
- Native crash diagnostics (VEH, hang watchdog, thread dump)
- 280+ C++ tests passing

### B. Partially Working
- Inspector (3 of 14 sections)
- ActionsSection (2 of 9 functional buttons)
- Search pill (UI only, no filtering logic)
- Sidebar navigation (4 of 20 items have content)

### C. Broken
- `main.cjs` only handles 5 of needed ~15 IPC channels
- Preload bridge missing `appState`, `thumbnail`, `gallery`, `window:*`, `app:quit`
- 8 electron modules exist but are never imported

### D. Dead Code
- `ShadcnGallery.jsx` (801 lines) — imported only by dead "shadcn" tab
- `GalleryGrid.jsx` (73 lines) — never imported
- 10 inspector section files — never imported by Inspector.jsx
- 8 electron modules — never imported by main.cjs
- `useAppState.jsx` — no consumers
- `thumbnail.js` — no consumers
- `ImageRenderer` — stub (compositor only)
- `ShaderRenderer` — fully implemented, no entry point
- `AudioVisualizerRenderer` — fully implemented, no entry point

### E. Missing
- Window state persistence
- Tray icon
- Close-to-tray
- Single-instance lock
- Supervisor/watchdog for Electron
- Crash recovery
- Gallery persistence
- Thumbnail persistence
- AppStateManager persistence
- Electron V8 memory cap
- GPU switch gating

### F. Surviving Untracked Implementation Artifacts
See Section 19 — 29 files totaling ~2,500+ lines of production code

### G. High-Risk Areas
- `main.cjs` reconstruction — the central integration point
- Cross-adapter shared handles — must match D3D11/D3D12 adapters
- Device loss recovery — complex save/restore/reload sequence
- WorkerW Explorer restart — timing-sensitive reacquisition
- Video pipeline frame scheduling — SPSC ring timing

### H. Low-Risk Quick Wins
- Import 10 inspector sections into Inspector.jsx
- Import GalleryGrid.jsx into WallpaperGallery.jsx
- Add V8 `--max-old-space-size=128` flag to main.cjs
- Gate GPU switches behind `REX_PREFER_IGPU`

### I. Architecture That Should NOT Be Changed
- Length-prefixed JSON protocol between Electron and native runtime
- D3D11VA → shared handle → D3D12 zero-copy pipeline
- SPSC lock-free FrameScheduler
- Per-monitor MonitorCompositor with D3D12Renderer adoption
- EDID-based monitor identity and persistence

### J. Architecture That Is Safe to Extend
- Inspector sections (add imports)
- Sidebar navigation (add tab content)
- Preload bridge (add channels)
- main.cjs IPC handlers (add handlers)
- Wallpaper type detection (add to registry.js)

## 24. REBUILD ROADMAP

### P0 — Required for Existing Functionality

| Task | Files | Change | Dependencies | Risk |
|------|-------|--------|-------------|------|
| Wire AppStateManager into main.cjs | main.cjs | Import, instantiate, add 4 IPC handlers | None | Low |
| Add appState bridge to preload.cjs | preload.cjs | Add `appState: { get, set, update, flush }` | AppStateManager wiring | Low |
| Wire WallpaperLibrary into main.cjs | main.cjs | Import, instantiate (self-registers gallery.* IPC) | None | Low |
| Add gallery bridge to preload.cjs | preload.cjs | Add `gallery: { add, remove, update, list, get }` | WallpaperLibrary wiring | Low |
| Add thumbnail bridge to preload.cjs | preload.cjs | Add `saveThumbnail`, `deleteThumbnail` | None | Low |
| Wire tray into main.cjs | main.cjs | Import TrayManager, create tray, wire callbacks | None | Low |
| Add window control IPC to main.cjs | main.cjs | `ipcMain.on('window:*')` handlers | None | Low |
| Add wallpaper:pause/resume to main.cjs | main.cjs | 2 IPC handlers | None | Low |

### P1 — Important Reliability

| Task | Files | Change | Dependencies | Risk |
|------|-------|--------|-------------|------|
| Wire Supervisor into main.cjs | main.cjs | Import, create deps, start watchdog | RuntimeManager, StateStore, CrashReporter, Minidump | Moderate |
| Add V8 memory cap | main.cjs | `app.commandLine.appendSwitch('js-flags', '--max-old-space-size=128')` | None | Low |
| Add GPU switch gating | main.cjs | `REX_PREFER_IGPU` env check | None | Low |
| Window state persistence | main.cjs | Save/restore position, size, maximized | AppStateManager | Low |
| Close-to-tray | main.cjs | Intercept close, hide window | TrayManager | Low |
| Add renderer-ping handler | main.cjs | `ipcMain.on('renderer-ping')` → supervisor | Supervisor | Low |

### P2 — Important Features

| Task | Files | Change | Dependencies | Risk |
|------|-------|--------|-------------|------|
| Mount 10 inspector sections | Inspector.jsx | Add imports | None | Trivial |
| Import GalleryGrid | WallpaperGallery.jsx | Replace inline grid | None | Low |
| Wire useAppState consumers | App.jsx, components | Use the hook | preload bridge | Low |
| Wire thumbnail.js consumers | store.js, WallpaperGallery.jsx | Import and call | preload bridge | Low |
| Add RuntimeSettings to CMakeLists.txt | CMakeLists.txt | Add to source list | RuntimeSettings.cpp exists | Low |

### P3 — Optional/Future

| Task | Files | Change | Dependencies | Risk |
|------|-------|--------|-------------|------|
| Wire ShaderRenderer apply path | registry.js, desktop.js, main.cjs, preload.cjs | Add detection, IPC, LOAD type | ShaderRenderer exists | Moderate |
| Wire AudioVisualizerRenderer | registry.js, desktop.js, main.cjs, preload.cjs | Add detection, IPC, LOAD type | AudioVisualizerRenderer exists | Moderate |
| Full inspector functionality | Inspector sections | Wire property bindings to store | AppStateManager | Moderate |
| Personalization module exposure | C++ modules → Electron IPC | Create IPC handlers for ThemeManager etc. | C++ modules exist | Significant |

## 25. MACHINE-READABLE SUMMARY

```json
{
  "working": [
    "image_wallpaper_apply",
    "video_wallpaper_apply",
    "gallery_file_import",
    "gallery_grid_display",
    "gallery_sort",
    "gallery_context_menu",
    "preview_css_filters",
    "media_controls_overlay",
    "playlist_engine_23_modes",
    "runtime_event_bridge",
    "desktop_apply_bridge",
    "runtime_telemetry_22_fields",
    "native_crash_diagnostics",
    "280_plus_cpp_tests"
  ],
  "partial": [
    "inspector_3_of_14_sections",
    "actions_section_2_of_9_buttons",
    "search_pill_ui_only",
    "sidebar_4_of_20_items"
  ],
  "broken": [
    "main_cjs_only_5_ipc_handlers",
    "preload_missing_appState_bridge",
    "preload_missing_gallery_bridge",
    "preload_missing_thumbnail_bridge",
    "preload_missing_window_controls",
    "8_electron_modules_dead_code"
  ],
  "dead_code": [
    "ShadcnGallery.jsx",
    "GalleryGrid.jsx",
    "10_inspector_section_files",
    "8_electron_modules_supervisor_tray_library_appstate",
    "useAppState.jsx",
    "thumbnail.js",
    "ImageRenderer_stub",
    "ShaderRenderer_no_entry",
    "AudioVisualizerRenderer_no_entry"
  ],
  "missing": [
    "window_state_persistence",
    "tray_icon",
    "close_to_tray",
    "single_instance_lock",
    "supervisor_watchdog_electron",
    "crash_recovery_electron",
    "gallery_persistence",
    "thumbnail_persistence",
    "appstate_manager_persistence",
    "v8_memory_cap",
    "gpu_switch_gating"
  ],
  "surviving_untracked": [
    "app-state-manager.cjs (195 lines, NOT WIRED)",
    "tray-manager.cjs (111 lines, NOT WIRED)",
    "wallpaper-library.cjs (181 lines, NOT WIRED)",
    "supervisor/ (5 files, 579 lines, NOT WIRED)",
    "RuntimeSettings.cpp/.hpp (243 lines, INTEGRATED in C++)",
    "useAppState.jsx (31 lines, bridge MISSING)",
    "thumbnail.js (94 lines, bridge MISSING)",
    "GalleryGrid.jsx (73 lines, NOT IMPORTED)",
    "10_inspector_sections (463 lines, NOT IMPORTED)",
    "MediaControlsOverlay.jsx (1050 lines, ACTIVE)",
    "PlaylistEngineDialog.jsx (471 lines, ACTIVE)",
    "DisplaySection.jsx (76 lines, ACTIVE)",
    "runtimeEvents.js (35 lines, ACTIVE)",
    "desktop.js (97 lines, ACTIVE)",
    "mediaBridge.js (17 lines, ACTIVE)",
    "23_powershell_test_scripts"
  ],
  "critical_files": [
    "app/electron/main.cjs (179 lines, MISSING most IPC)",
    "app/electron/preload.cjs (24 lines, MISSING bridge channels)",
    "app/electron/wallpaperRuntime.cjs (155 lines, ACTIVE)",
    "app/src/App.jsx (72 lines, ACTIVE)",
    "app/src/wallpapers/store.js (164 lines, ACTIVE)",
    "app/src/wallpapers/desktop.js (97 lines, ACTIVE)",
    "app/src/wallpapers/DesktopApply.jsx (62 lines, ACTIVE)",
    "app/src/wallpapers/WallpaperGallery.jsx (663 lines, ACTIVE)",
    "app/src/wallpapers/inspector/Inspector.jsx (131 lines, INCOMPLETE)",
    "rex-core/src/wallpaper_cli_main.cpp (2159 lines, NATIVE ENTRY)",
    "rex-core/src/wallpaper/engine/Engine.cpp (1167 lines, CORE ENGINE)",
    "rex-core/src/wallpaper/decoder/FfmpegVideoDecoder.cpp (925 lines, VIDEO DECODE)",
    "rex-core/src/wallpaper/video/VideoManager.cpp (474 lines, VIDEO MGMT)"
  ],
  "critical_bugs": [
    "main_cjs_only_5_of_15_needed_ipc_handlers",
    "preload_cjs_missing_4_bridge_channels",
    "8_electron_modules_exist_but_never_imported",
    "inspector_11_of_14_sections_not_mounted",
    "appstate_persistence_fully_coded_but_not_wired"
  ],
  "next_p0": [
    "wire_appstate_manager_into_main_cjs",
    "add_appstate_bridge_to_preload_cjs",
    "wire_wallpaper_library_into_main_cjs",
    "add_gallery_bridge_to_preload_cjs",
    "add_thumbnail_bridge_to_preload_cjs",
    "wire_tray_into_main_cjs",
    "add_window_control_ipc_to_main_cjs",
    "add_wallpaper_pause_resume_to_main_cjs"
  ],
  "next_p1": [
    "wire_supervisor_into_main_cjs",
    "add_v8_memory_cap",
    "add_gpu_switch_gating",
    "window_state_persistence",
    "close_to_tray",
    "add_renderer_ping_handler"
  ]
}
```

---

**CURRENT STATE AUDIT COMPLETE — NO FILES MODIFIED.**
