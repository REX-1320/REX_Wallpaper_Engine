# REX Project Status

> Last updated: 2026-07-25
> Auto-generated from codebase audit. Update this file when a feature reaches ✅.

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
| WinExplorerAdapter | ✅ | Reads/writes explorer registry keys |
| WinStartMenuAdapter | ✅ | Reads/writes start menu registry keys |
| PersonalizationPage | ✅ | Full workspace: Toolbar (Apply/Reset), Tabs (Taskbar/Explorer/StartMenu), Toggles, Dropdowns, Slider, PropertyGrid inspector |

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
