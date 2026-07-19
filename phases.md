# REX Wallpaper Engine -- Next Development Phases

## Guiding Principles

### Do

-   Preserve current architecture.
-   Build incrementally.
-   Test every phase.
-   Keep modules independent.
-   Keep public APIs stable.

### Do NOT

-   Redesign rex-core.
-   Duplicate renderer/decoder.
-   Mix UI with engine logic.
-   Expose Win32 internals outside desktop hosting.

------------------------------------------------------------------------

# Phase 1 -- Static Image Wallpapers

Objectives: - PNG/JPG/WEBP/BMP - Fill/Fit/Stretch/Center - Texture
cache - Hot reload

Architecture: - wallpaper/loader - wallpaper/scene - graphics/resource

Exit: Images render correctly.

------------------------------------------------------------------------

# Phase 2 -- Video Wallpapers

Objectives: - MP4 via Media Foundation - Loop/Pause/Resume - Frame
sync - Decoder thread

Pipeline: Decoder → Playback → GPU Upload → Renderer

Exit: Smooth playback.

------------------------------------------------------------------------

# Phase 3 -- Wallpaper Library

-   Import
-   Metadata
-   Thumbnails
-   Favorites
-   Collections

------------------------------------------------------------------------

# Phase 4 -- Multi-Monitor

-   Independent wallpaper
-   Hot-plug recovery
-   DPI aware
-   Shared GPU resources

------------------------------------------------------------------------

# Phase 5 -- Performance

-   Pipeline cache
-   Texture cache
-   Upload heap reuse
-   Descriptor heap reuse
-   GPU profiling

------------------------------------------------------------------------

# Phase 6 -- Wallpaper SDK

Interfaces: - IWallpaperEngine - IWallpaperScene - IWallpaperLoader

Never expose Win32 handles.

------------------------------------------------------------------------

# Phase 7 -- Widgets

Wallpaper → Compositor → Widgets → Desktop

Widgets must render through the compositor.

------------------------------------------------------------------------

# Phase 8 -- Effects

-   Blur
-   Acrylic
-   Mica
-   Particles
-   Shader effects

------------------------------------------------------------------------

# Phase 9 -- Plugin System

-   Stable SDK
-   Sandboxed plugins
-   Version compatibility

------------------------------------------------------------------------

# Phase 10 -- Production

-   Tests
-   Leak detection
-   Explorer recovery
-   Crash recovery
-   CI/CD
-   Installer
-   Documentation

## Final Architecture

rex-core/ - core/ - graphics/ - wallpaper/ - widget/ - ipc/ - plugins/

Dependencies: Wallpaper → Graphics Widgets → Graphics Graphics →
Direct3D 12 Core owns lifecycle.
