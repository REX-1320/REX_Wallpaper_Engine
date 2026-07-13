# REX Tools — RES Documentation Package (v1.2)

> Engineering specification set. Companion to the Master Vision document.
> All modules now at full/executable spec depth.

## Document Index

| # | Document | Version | Depth | Status |
|---|---|---|---|---|
| 1 | `ADR-001-to-006.md` | v1.0 | Complete | Locked decisions |
| 2 | `RES-Widget-Platform.md` | v1.1 | Complete | Executable — Phase 1 |
| 3 | `RES-Desktop-Platform.md` | v1.0 | Complete | Executable — Phase 1 |
| 4 | `RES-SDK-Marketplace.md` | v1.1 | Complete | Executable — Phase 1/2/5 |
| 5 | `RES-Wallpaper-Platform.md` | v1.0 | Complete | Executable — Phase 3 |
| 6 | `RES-WindowFX.md` | v1.0 | Complete | Executable — Phase 3 |
| 7 | `RES-Automation-Engine.md` | v1.0 | Complete | Executable — Phase 4 |
| 8 | `RES-Rendering-Layer.md` | v1.0 | Complete | Executable — cross-cutting, hardens through Phase 3 |
| 9 | `RES-Phase-0-1-Implementation.md` | v1.1 | Complete | Step-by-step, start Monday |
| 10 | `RES-Testing-Security-Performance.md` | v1.0 | Complete | Executable |
| 11 | `RES-API-Reference.md` | v1.0 | Complete (core) | Extend as SDK grows |

## Reading Order

**To start building today:** 9 → 1 → 2 → 3 → 4 (§2.1) → 10
**To understand the whole system:** 1 → 2 → 3 → 4 → 5 → 6 → 7 → 8 → 11
**Before writing any plugin-facing or automation-command code:** doc 10 (security section) and doc 7 §5 (RunCommandAction trust tier) are mandatory reading first.

## Cross-Module Dependency Graph

```
ADR-001..006 (foundation)
    |
    +--> RES-Widget-Platform -----+--> RES-SDK-Marketplace
    |         |                   |         |
    |         v                   |         v
    +--> RES-Desktop-Platform     |    RES-Wallpaper-Platform --+
    |         |                   |         |                   |
    |         v                   |         v                   v
    |    RES-WindowFX <-----------+    RES-Rendering-Layer <-----+
    |         |                                |
    |         +----------------+---------------+
    |                          v
    +-------------------> RES-Automation-Engine  (last -- depends on everything else's event surface)

RES-Testing-Security-Performance and RES-API-Reference are cross-cutting,
referenced by all of the above.
```

## Note on Revision

Docs 5-8 (Wallpaper/WindowFX/Automation/Rendering) were expanded from an
earlier architectural-outline pass to full executable depth. Several sections
explicitly flag open questions to validate empirically at each phase's spike
(e.g. RES-WindowFX section 4.1's Windows-version effect-path validation,
RES-Wallpaper-Platform section 7's GPU-capable CI runner requirement,
RES-Rendering-Layer section 5's triple-vs-double-buffering latency check) --
"full depth" here means fully specified decisions with named validation
steps, not claims that every number is pre-verified. Treat flagged items as
the first thing to confirm when that phase starts.

## Versioning

Each RES doc is independently versioned (RES-x.y). Breaking changes to a
module's spec require a version bump and a changelog entry at the top of that
file. Cross-references between docs use [RES-Widget 3.2] style anchors.
