# AGENTS.md — REX standing rules

## Your role

**Main job: make the wallpaper engine perfect — find and fix bugs in the wallpaper path first:**

- `rex-core/` wallpaper stack (engine, decoders, D3D12, WorkerW, playback, runtime protocol)
- `app/` gallery + apply path (only the parts that put image/video on the desktop)

Widgets, taskbar, themes, WindowFX, marketplace, automation, SDK, editors: **deferred — do not spend time on them** until the engine draws increasing users (see TODO.md scope banner).

Feature work is secondary. When a bug and a feature compete, fix the bug.

## Mandatory: update the tracking docs after EVERY finished work unit

No exceptions. A work unit is complete only when both files are updated:

1. **`TODO.md`** — check off / remove the item you finished; add anything newly discovered (especially bugs) to the 🐛 Bug backlog.
2. **`everything.md`** — move the result into the done inventory (✅ section), update the scoreboard counts and the "Last updated" date.

Also update `#project_status.md` when a *feature* ships (per its own convention), and `README.md`'s status line if overall standing changes.

## Bug lifecycle

1. **Discover** → add to `TODO.md` 🐛 Bug backlog with file/log references
2. **Diagnose** → root cause first, no guessing (systematic-debugging: reproduce, evidence, hypothesis)
3. **Fix + verify** → build/test the affected layer (commands below)
4. **Close** → check off in `TODO.md`, add to `everything.md` ✅ done inventory

## Build & verify

```powershell
# Electron app
pnpm dev            # run UI
pnpm lint           # oxlint
pnpm typecheck      # tsc (root)

# Native core
cmake --build rex-core/build
cd rex-core && ctest
```

## Key files

| File | Purpose |
|---|---|
| `TODO.md` | Remaining work + 🐛 bug backlog (living) |
| `everything.md` | Done inventory + repo map (living) |
| `#project_status.md` | Per-feature ✅/🟡/⬜ audit |
| `fix.md`, `hdr_*.log` | Active investigations (HDR present bug) |
| `docs/res/00_INDEX.md` | Spec index |

## Gotchas

- Native C++ shell UI is **retired** — archived in `dump/`. Don't revive it; current UI is Electron (`app/`).
- Uncommitted engine changes may exist (`git status` first) — `D3D12Renderer`, `WorkerWManager` were WIP.
- `#project_status.md` "Shell UI" section is stale (July audit).
