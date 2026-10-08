# REX Tools

> A modern desktop platform for Windows — widgets, wallpaper, window effects, and automation.

**Status:** Active development — native core engine + Electron shell functional (well past Phase 0)
**Target hardware:** Windows 10/11
**See also:** [everything.md](everything.md) (what exists & what's done) · [TODO.md](TODO.md) (what's left) · [#project_status.md](%23project_status.md) (per-feature audit)

---

## Overview

REX is the layer between Windows and the user that Windows never built: fast, native, deeply customizable, with a real plugin economy.

- **Widgets** — live info on your desktop (clock, weather, system monitor)
- **Wallpaper** — static, video, web, interactive, audio-reactive
- **WindowFX** — blur, acrylic, mica, shadows, per-app rules
- **Automation** — event-driven workflows tying it all together
- **SDK** — build widgets with HTML/CSS/JS
- **Marketplace** — discover, install, share community content

## What's done already

~121 features shipped (~24 partial, ~164 to go — see [TODO.md](TODO.md)). Highlights:

- **Wallpaper engine:** 6 renderers (image / video via MF+ffmpeg / web / D3D12 shader / scene / audio-reactive), playback controls, telemetry, multi-monitor, recovery
- **Widgets:** layered-HWND host + Clock, Weather, System Monitor built-ins
- **Personalization:** taskbar/explorer/start-menu registry adapters + full manager/service stack, profiles with import/export/rollback
- **Module platform:** loader, lifecycle, health monitor, sandbox, signature verification, dependency resolver, event bus
- **Theme system:** registry, tokens, style resolver, component registry
- **UI:** Electron + React app (wallpaper gallery, widgets workspace, installer packaging) + TS Surface System with 6 materials and design tokens (earlier native C++ shell archived in `dump/`)
- **Docs:** complete RES spec suite (16 docs), ADRs 001–006, developer guide
- Full inventory: [everything.md](everything.md)

## Architecture

```
Core (C++20) ←── IPC (named pipes + protobuf) ──→ Shell (C# / WinUI 3)
  • WorkerW host                                 • Widget Manager
  • GPU compositor                               • Settings UI
  • Animation engine                             • Package Manager
  • Render scheduler                             • Automation Editor
  • WindowFX renderer                            • Marketplace Client
```

*Core language decision: C++20 now, Rust deferrable post-Phase 3 per ADR-006.*
*Current UI: Electron + React (`app/`) + TS Surface System (`src/ui/surface`); C# shell in `rex-shell/`; retired native C++ UI in `dump/`.*

## Docs

| Doc | What it covers |
|---|---|
| [ADR-001–006](docs/res/ADR-001-to-006.md) | All architecture decisions: stack, IPC, sandbox, persistence, telemetry, language |
| [00_INDEX](docs/res/00_INDEX.md) | Full document index and reading order |
| [RES-Widget-Platform](docs/res/RES-Widget-Platform.md) | Widget lifecycle, SDK surface, data sources, rendering contract |
| [RES-Desktop-Platform](docs/res/RES-Desktop-Platform.md) | WorkerW host, multi-monitor, profiles, scenes, backup |
| [RES-SDK-Marketplace](docs/res/RES-SDK-Marketplace.md) | Widget packaging, CLI, permissions, code signing, marketplace API |
| [RES-Wallpaper-Platform](docs/res/RES-Wallpaper-Platform.md) | Wallpaper types, video decode, audio-reactive, multi-monitor sync |
| [RES-WindowFX](docs/res/RES-WindowFX.md) | Window effects, rule engine, per-app profiles, DWM APIs |
| [RES-Automation-Engine](docs/res/RES-Automation-Engine.md) | Workflow engine, event bus, triggers/actions, flow editor |
| [RES-Rendering-Layer](docs/res/RES-Rendering-Layer.md) | GPU compositor, layer model, frame budget, animation engine |
| [RES-Phase-0-1-Implementation](docs/res/RES-Phase-0-1-Implementation.md) | Step-by-step build plan for Phases 0 and 1 |
| [RES-Testing-Security-Performance](docs/res/RES-Testing-Security-Performance.md) | CI pipeline, threat model, performance SLAs, definition of done |
| [RES-API-Reference](docs/res/RES-API-Reference.md) | IPC protobuf schema, C# interfaces, TypeScript SDK, CLI reference |
| [Getting Started](docs/developer-guide/getting-started.md) | Prerequisites, build, run, debug |
| [Coding Conventions](docs/developer-guide/coding-conventions.md) | C++ + C# style, MVVM, git, review checklist |
| [ADR Process](docs/developer-guide/adr-process.md) | When/how to write ADRs |
| [Testing Guide](docs/developer-guide/testing-guide.md) | Test pyramid, examples, CI config |

## Roadmap

| Phase | Focus | Exit criteria |
|---|---|---|
| **0** | Foundation — WorkerW host, IPC, CI | Colored rectangle appears behind icons, survives explorer restart |
| **1** | Widget platform MVP — host, 3 widgets, themes | You use it daily instead of Rainmeter |
| **2** | SDK alpha — API, CLI, sandbox, docs | Stranger builds a widget from docs alone |
| **3** | Wallpaper + WindowFX — video/web wallpaper, effects | Feature parity with Rainmeter + Wallpaper Engine |
| **4** | Automation engine — workflows, flow editor | "Battery low → change wallpaper → notify" end-to-end |
| **5** | Public marketplace — publishing, install, review | 3rd-party plugin installed safely without your involvement |
| **6** | Platform maturity — diagnostics, themes, a11y, i18n | Self-sustaining community platform |

## Quick Start

```powershell
git clone <repo-url> rex
cd rex

# Build core
cmake -S rex-core -B rex-core/build
cmake --build rex-core/build

# Build shell
dotnet build rex-shell/REX.sln

# Run tests
cd rex-core && ctest
cd ../rex-shell && dotnet test
```

---

*Built by one person, AI-assisted, disciplined enough to look like it came from a team.*
