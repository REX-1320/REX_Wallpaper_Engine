# REX Tools --- Master Vision & Project Overview

> **Status:** Living document (v0.3)
> **Author:** Solo developer, AI-assisted (opencode)
>
> This document captures the long-term vision, goals, requirements,
> scope, and guiding principles for REX Tools. It is the product
> reference, not the engineering specification. Engineering detail
> lives in per-subsystem RES documents (see §14, `docs/res/`).
>
> **Architecture update (v0.3):** Core language finalized as C++20
> (per ADR-006), with a scheduled re-evaluation at Phase 3 exit for a
> potential Rust rewrite. RES engineering specs in `docs/res/` supersede
> the earlier module-outline documents — refer to those for
> implementation-level detail.

------------------------------------------------------------------------

# 1. Vision & Mission

REX Tools aims to become a modern desktop platform rather than a single
application — the layer between Windows and the user that Windows
itself never built: fast, native, deeply customizable, and built around
a real plugin economy instead of a scripting language nobody wants to learn.

**Mission statement:** Give power users and creators a native Windows
platform for shaping their desktop — widgets, wallpaper, window
behavior, and automation — with the performance of native code and the
extensibility of a real SDK, without the bloat, jank, or abandoned
scripting languages of the tools that came before it.

Primary philosophy:

- Native-first
- Modular
- Fast
- Stable
- Extensible
- Beautiful
- Long-term maintainable
- **Buildable by one person, scalable to a community**

------------------------------------------------------------------------

# 2. Product Goals & Differentiation

## Core Goals

- Better desktop experience than existing customization tools.
- Native Windows integration.
- Production-quality architecture.
- Modular subsystems.
- Stable public APIs.
- Powerful SDK.
- Plugin ecosystem.
- Long-term scalability.

## Why REX, specifically (competitive positioning)

"Better than existing tools" only means something if you name the gap.
Rough map of where today's tools fall short — this is the wedge REX should
sharpen, not just a comparison table:

| Tool | Where it's strong | Where it struggles |
|---|---|---|
| Rainmeter | Deep customization, huge community skins | Steep scripting-language learning curve, dated GUI editor, no real plugin marketplace |
| Wallpaper Engine | Great wallpaper content, huge Workshop | Not a widget/automation platform; can be heavy on resources |
| Widget Launcher / Rocketdock-likes | Simple, lightweight | Shallow — no automation, no real SDK, minimal theming depth |
| KDE Plasma | Genuinely modular widget shell | Linux-only; irrelevant to Windows users directly, but a good architecture reference |

REX's wedge: **a real widget SDK with a modern language/toolchain (not a
bespoke scripting DSL), a visual editor good enough that most users never
touch code, and an automation engine that ties widgets/wallpaper/window
behavior together as one system rather than three unrelated apps.**
That combination is currently nobody's whole product.

------------------------------------------------------------------------

# 3. Inspirations (Not Copies)

The project studies ideas from:

- Rainmeter
- Wallpaper Engine
- KDE Plasma
- Windows Shell
- macOS Desktop
- Smart Launcher
- Modern dashboard systems
- **Obsidian / VS Code** — for plugin API + marketplace design done well in a dev-tool context
- **Stream Deck / Elgato software** — for a plugin permission and profile model that non-technical users still trust

Ideas may inspire REX, but implementations should be original.

------------------------------------------------------------------------

# 4. Technical Architecture & Stack Strategy (NEW)

For a solo, AI-assisted developer, the stack decision matters more than
almost anything else in this document — it determines how much of the
"enterprise-grade" ambition in §7 is actually reachable alone.

**Final decision (ADR-001, ADR-006):**

- **Core engine / rendering / compositor:** C++20 (MSVC, CMake) for the
  desktop host, WorkerW integration, and GPU compositor — this is where
  performance and stability live, and where bugs are most expensive.
  Rust is deferred to a post-Phase 3 re-evaluation; the IPC boundary
  (ADR-002) makes a language swap feasible without touching the Shell.
- **Application shell, widget host, SDK surface, settings UI:** C# /
  .NET 8 + WinUI 3 — drastically faster to build and iterate on solo,
  huge ecosystem, AI coding assistants strongest here.
- **Widget authoring layer:** web-based (HTML/CSS/JS via WebView2 or a
  minimal embeddable renderer, per RES-SDK-Marketplace §2) — unlocks a
  real community since far more people can write a widget in web tech
  than in C++/C#.
- **IPC between native core and shell:** named pipes + protobuf
  (ADR-002), two duplex pipes (control + high-frequency) — the stable,
  versioned API boundary that lets both halves evolve independently.

**Why this split matters for a solo dev:** it lets you move fast in the
90% of the app that's UI/business logic, while keeping the 10% that's
genuinely performance-critical isolated, small, and rewritable without
touching the rest of the platform.

**See also:** ADR-001 (stack decision), ADR-006 (C++ now, Rust later)
in `docs/res/ADR-001-to-006.md`.

------------------------------------------------------------------------

# 5. Major Modules

## Widget Platform

- Native desktop widgets
- Drag & drop, resize, lock, pin to desktop
- Grouping, snapping
- Widget SDK
- Marketplace
- Theme support
- **Data source abstraction layer** (NEW) — widgets bind to "data
  sources" (CPU/RAM, weather, RSS, media session, custom HTTP/plugin)
  rather than hardcoding sensors, so one widget can be re-skinned
  against many data types
- **Visual widget editor** (NEW) — drag-and-bind UI so non-coders can
  build widgets without touching the SDK; this is the actual growth
  lever for a plugin ecosystem

## Desktop Platform

- WorkerW desktop host
- Multi-monitor, layouts, profiles, scenes
- Workspace switching
- Desktop icon management
- Backup & restore
- **Cloud/file-based profile sync** (NEW) — export/import profiles as
  portable files first; real cloud sync can come later once there's a
  backend to justify it

## Wallpaper Platform

- Static, video, web, interactive, dynamic
- Multi-monitor synchronization
- **Audio-reactive layer** (NEW) — system audio → wallpaper/widget
  parameter binding, a well-loved feature category (Wallpaper Engine
  users specifically request this) that's cheap to add once the data
  source abstraction in Widget Platform exists

## WindowFX

- Blur, acrylic, Mica, glass, shadows, animations
- Window rules
- **Per-app profiles** (NEW) — window effect rules scoped to specific
  apps or app categories, not just global settings

## Automation

- Workflow engine, triggers, conditions, actions
- Scheduling, history, logging, permissions
- **Visual flow editor** (NEW) — node-based UI (see: Automator's flowchart, 
  n8n, Shortcuts) so automations aren't just config files
- **Event bus** (NEW) — the automation engine subscribes to the same
  event stream widgets and window rules use, so "widget click → trigger
  automation → change wallpaper" is one system, not three

## Rendering

- GPU acceleration, animation engine, render scheduler, layer composition
- **Render budget / frame governor** (NEW) — a hard performance
  contract (e.g. widgets get a shared frame-time budget) so one bad
  community widget can't tank the whole desktop's frame rate — critical
  once third-party plugins exist

## SDK

- Plugin API, Widget API, templates, CLI, packaging, versioning
- **Sandboxed plugin runtime** (NEW, see §9)
- **Local dev server + hot reload** (NEW) — since you're the primary
  SDK user for a long while, invest early in your own developer loop

## Diagnostics & Telemetry (NEW module)

- Local performance dashboard (frame time, memory, per-widget cost)
- **Opt-in, anonymized crash + usage telemetry** — genuinely important
  for a solo dev with no QA team; this is how you find out about bugs
  you'd otherwise never see
- Crash reporter with local repro capture

------------------------------------------------------------------------

# 6. Core Principles

- One source of truth.
- Explicit ownership.
- Event-driven communication.
- Stable APIs.
- Native integration behind abstractions.
- Security by design.
- Performance is a feature.
- Backward compatibility whenever practical.
- **Boring technology where it doesn't matter, novel technology only where it's the point** (NEW) — spend novelty budget on rendering/compositing, not on reinventing config file formats or build tooling.

------------------------------------------------------------------------

# 7. Target Quality

- Enterprise-grade architecture
- Production-ready codebase
- Excellent developer experience
- High performance
- Extensible design
- Strong testing culture
- **Calibration note (NEW):** "enterprise-grade" for a solo dev means
  *disciplined*, not *heavyweight* — automated tests and CI matter more
  here than they would on a big team, because you have no second pair
  of eyes catching regressions. See §10.

------------------------------------------------------------------------

# 8. Desired User Experience

- Smooth animations
- Modern glass aesthetics
- Minimal latency
- Fast startup
- Reliable persistence
- Rich customization
- Intuitive interaction
- **Zero-config first run** (NEW) — the app should look good with
  nothing installed; empty-state design is where most customization
  tools lose new users in the first 60 seconds

------------------------------------------------------------------------

# 9. Security, Trust & Plugin Sandboxing (NEW)

Once a marketplace and SDK exist, this section stops being optional.

- **Permission manifest per plugin** — plugins declare what they need
  (network, filesystem, system info) up front; users see this before
  install, same pattern as browser extensions.
- **Sandboxed execution** — widget/plugin code runs in a restricted
  runtime (e.g. isolated JS context or process) with no default
  filesystem/network access.
- **Code signing for marketplace submissions** — even a lightweight
  automated scan + signature beats nothing, and is table stakes for
  user trust once REX runs arbitrary community code.
- **Resource quotas** — CPU/memory/frame-time ceilings per plugin,
  enforced by the render budget in §5.
- **Review queue** — even a solo-maintained, semi-automated review
  pipeline for marketplace submissions, to avoid REX becoming a malware
  distribution vector on day one of having an install button.

This is the single biggest reputational risk in the whole plan if
skipped — a customization tool with a plugin store is a very attractive
target, and one bad incident early can be fatal to trust.

------------------------------------------------------------------------

# 10. Solo Developer Execution Model (NEW)

This is the section most vision docs skip, and the one that most
determines whether the rest of this document is real or aspirational.

- **AI-assisted workflow as a force multiplier, not a shortcut around
  discipline.** Use opencode/AI agents for scaffolding, boilerplate,
  test generation, and refactors — but architecture decisions (§4),
  security-sensitive code (§9), and public API surfaces (§2) get
  reviewed by you line-by-line, every time. AI-generated code moves
  fast and silently accumulates debt if unreviewed.
- **Write for future-you, not a future team.** Comment and document as
  if explaining to a stranger — because six months from now, you *are*
  the stranger. This is the actual mitigation for solo "bus factor" risk.
- **Automate the things a team would give you for free.** CI on every
  commit (build + tests + lint), even solo. A team gives you code
  review and CI by default; alone, you have to build that safety net
  deliberately or you won't have one.
- **Scope discipline via the Guiding Rule (§19).** The biggest solo-dev
  failure mode isn't lack of ambition, it's parallel half-finished
  modules. One module at a time to "done," not six modules at 60%.
- **Time-box architecture spikes.** Give yourself a hard deadline (e.g.
  1 week) to prototype the riskiest unknown (WorkerW + GPU compositor
  interaction) before committing to it in the ADR.
- **Public build-in-public cadence** (optional but powerful for solo
  devs) — devlogs force you to produce demonstrable progress on a
  cadence, which is a natural antidote to scope creep and disappearing
  into infrastructure work forever.

------------------------------------------------------------------------

# 11. Roadmap (Phased, replacing flat priority list)

A flat priority list doesn't tell you when to stop working on one thing
and start the next. Phases with exit criteria do.

**Phase 0 — Foundation (no user-facing features)**
- ADR-001: stack + architecture decision (§4)
- Repo scaffolding, CI pipeline, test harness
- Minimal WorkerW host that can render *one* static rectangle
- *Exit criteria: a widget-shaped box survives explorer.exe restart*

**Phase 1 — Widget Platform MVP**
- Widget host: drag, resize, lock, pin (from §5)
- 3-5 first-party widgets (clock, system monitor, weather) as SDK dogfood
- Basic theme support
- *Exit criteria: you personally use it daily instead of Rainmeter/nothing*

**Phase 2 — SDK Alpha (closed)**
- Public widget API, CLI, packaging
- Visual widget editor (basic version)
- Sandboxed runtime (§9) — non-negotiable before any external code runs
- Invite 5-10 alpha testers
- *Exit criteria: a stranger builds a working widget from docs alone, unassisted*

**Phase 3 — Wallpaper + WindowFX**
- Static/video/web wallpaper, multi-monitor sync
- Blur/acrylic/Mica, window rules
- *Exit criteria: feature parity with "why would I still use Rainmeter/WallpaperEngine"*

**Phase 4 — Automation Engine**
- Workflow engine, event bus tying widgets/wallpaper/window rules together
- Visual flow editor
- *Exit criteria: a demo automation ("battery low → change wallpaper → notify widget") works end-to-end*

**Phase 5 — Public Marketplace**
- Review queue, code signing, permission manifests
- Public launch
- *Exit criteria: 3rd-party plugin installed and running safely without your direct involvement*

**Phase 6 — Platform Maturity**
- Performance profiler, diagnostics dashboard, crash recovery
- Community themes, scene sharing, workspace templates
- Everything in the current "Living Backlog" (§17) becomes fair game here, not before

------------------------------------------------------------------------

# 12. Business Model & Sustainability (NEW)

An ambitious platform needs a funding story or it dies at the point
your free time runs out — worth deciding early, even loosely.

- **Free core, paid Pro tier** — widget platform + basic SDK free;
  advanced automation, premium first-party widget packs, or cloud
  sync as a paid tier (one-time or low-cost subscription).
- **Marketplace revenue share** — small cut of paid community
  plugins/themes, mirroring the Wallpaper Engine Workshop model, which
  funds review-queue maintenance (§9) without you personally vetting
  every submission for free forever.
- **Sponsorship/donations during pre-revenue phases** (GitHub Sponsors,
  Patreon) tied to the devlog cadence in §10 — funds time, not features.
- **Open-core licensing consideration** — core platform closed or
  source-available; SDK/plugin format open, so the ecosystem isn't
  gated by your codebase being public.

------------------------------------------------------------------------

# 13. Community & Growth Strategy (NEW)

- Early presence: a Discord/forum *before* public launch, seeded with
  alpha testers from Phase 2 — community infrastructure takes time to
  warm up and shouldn't be built the week of launch.
- Devlogs (written or video) on a fixed cadence — doubles as the
  build-in-public discipline from §10 and as free marketing.
- Launch channels when ready: r/Windows10, r/pcmasterrace,
  ProductHunt, Rainmeter/WallpaperEngine community crossover posts
  (respectfully — not as a "we're better" pitch, as a "this might
  interest you" one).
- Documentation-first for the SDK — a stranger's ability to build a
  widget from docs alone (Phase 2 exit criteria) is also your best
  growth mechanism.

------------------------------------------------------------------------

# 14. Versioning, Release & Spec Process

- Engineering specifications (**RES** documents in `docs/res/`) define
  implementation-level detail for each subsystem — this vision doc stays
  product-level. 10 RES documents + 6 ADRs are now at full spec depth.
- **Semantic versioning from v0.1** — even pre-release, consistent
  versioning makes it possible to talk about compatibility later.
- **One ADR per irreversible decision** (stack, IPC protocol, plugin
  sandbox model) — cheap now, invaluable when you don't remember *why*
  in eight months. ADRs 001–006 are accepted (see `docs/res/ADR-001-to-006.md`).

------------------------------------------------------------------------

# 15. Risk Register (NEW)

| Risk | Mitigation |
|---|---|
| Solo burnout / disappearing scope | Phased roadmap with hard exit criteria (§11); build-in-public cadence (§13) |
| Bus factor (only one person understands the codebase) | Document for future-you (§10); ADRs (§14) |
| Plugin ecosystem becomes a security incident | Sandboxing, permissions, review queue (§9) — non-negotiable before Phase 5 |
| Stack choice turns out wrong | Time-boxed architecture spikes before commitment (§10); IPC boundary keeps core/shell swappable (§4) |
| AI-assisted code quality drift | Manual review gate on architecture/security/public API code (§10) |
| No users at launch | Community seeding starts at Phase 2, not Phase 5 (§13) |

------------------------------------------------------------------------

# 16. Success Metrics (NEW)

Concrete enough to know if a phase actually succeeded, not just "shipped":

- Phase 1: personal daily-use (binary, but real)
- Phase 2: time for an unassisted stranger to ship a first widget (target: under an hour, from docs)
- Phase 3: side-by-side performance benchmark vs. Rainmeter/Wallpaper Engine (CPU/RAM at idle)
- Phase 5: plugin submissions per week; time-to-review
- Ongoing: crash-free session rate (via opt-in telemetry, §5)

------------------------------------------------------------------------

# 17. Living Backlog (Ideas) — post-Phase-6 territory

- Widget marketplace growth features (ratings, collections, curated packs)
- Community themes, scene sharing, workspace templates
- Advanced animation editor
- Widget inspector / devtools
- Performance benchmarks (public leaderboard for widget efficiency)
- Accessibility improvements (screen reader support for widget UI, high-contrast themes)
- Localization / i18n for SDK docs and UI
- Cross-platform research (future, low priority — Windows-native is the whole differentiator for now)

------------------------------------------------------------------------

# 18. Non-Goals

- Copying existing products.
- Bloated monolithic architecture.
- Unstable APIs.
- Platform-specific logic spread throughout the application.
- **Building all six major modules in parallel** (NEW) — see Roadmap, §11.
- **Skipping plugin security to hit a launch date** (NEW) — see §9.

------------------------------------------------------------------------

# 19. Guiding Rule

Every major feature should satisfy:

- Solves a real user problem.
- Fits the overall architecture.
- Can scale.
- Can be maintained.
- Can be tested.
- Does not compromise performance.
- **Can one person actually build and maintain this in the current phase, with AI assistance, without stalling everything else** (NEW) — the solo-dev-specific filter that everything above this line exists to support.

------------------------------------------------------------------------

# 20. Long-Term Vision

REX Tools should evolve into a desktop platform that supports multiple
independent subsystems while maintaining a unified architecture and user
experience — built by one person, disciplined enough to look like it
came from a team, and open enough in its SDK to eventually become one.
