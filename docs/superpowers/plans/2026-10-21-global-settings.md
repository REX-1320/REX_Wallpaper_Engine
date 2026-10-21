# REX Global Settings (Tier A) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give REX a real global Settings page, and put the engine's already-live
colour controls plus its already-measured telemetry in front of the user.

**Architecture:** Three independent slices, each shippable on its own.

1. **Picture group (A2)** — the runtime already accepts `SET_BRIGHTNESS` /
   `SET_CONTRAST` / `SET_SATURATION` and applies them per frame; the app simply
   never sends them. This is app-side plumbing only: capability map → IPC →
   per-file persistence → an Inspector section. The colour values are stored
   per wallpaper (gallery library, percent scale 0–200, 100 = unchanged) and
   mirrored into `last-wallpaper.json` on the engine scale (0–2), exactly the
   split the existing `volume` property already uses.
2. **Settings page (A1 + A4 + A5)** — a new `SettingsPage` rendered for
   `tab === 'Settings'` in `App.jsx`, plus window-bounds persistence and a
   close-behaviour toggle. Window-bounds resolution is a pure module with
   `node:test` coverage (a window restored onto a disconnected monitor must
   not open off-screen).
3. **Diagnostics (A3)** — one new `wallpaper:status` IPC that reports the
   runtime's own `GET_STATUS` telemetry, formatted by a pure module.

**Tech Stack:** Electron 43 main process (CommonJS `.cjs`), React 19 + Vite
renderer, `node:test` for pure modules, no DOM test runner in this repo.

**Spec:** [`research-settings.md`](../../research-settings.md) — Tier A (§5
Tier A), with the per-item evidence in §2 and the dead surface in §4.

---

## Global constraints

1. **No engine change, no engine rebuild.** The shipped Release runtime
   (`rex-core/.build/Release/rex-wallpaper-runtime.exe`, 2026-10-21 18:36)
   already contains `SET_BRIGHTNESS`, `SET_CONTRAST`, `SET_SATURATION` and
   `GET_STATUS` (verified by scanning the binary's command strings). `rex-core`
   is not touched, not rebuilt, not recompiled.
2. **Never spawn a runtime to answer a question.** `WallpaperRuntime.send()`
   calls `_spawn()` when no process exists. `wallpaper:status` must therefore
   check `runtime.proc` itself and report `unavailable` instead of starting an
   engine (and its VRAM) because someone opened a page.
3. **Capability-gated, always.** A control renders only when the runtime's
   `GET_CAPABILITIES` advertises the property for the *current* adapter. The
   static (image) adapter does not support the colour knobs
   (`RendererAdapters.cpp:50-52`), so the Picture group is video-only and must
   be gated, not assumed.
4. **Two scales, one owner.** App scale = percent 0–200 (100 = unchanged).
   Engine scale = factor 0–2 (1.0 = identity). The renderer converts, exactly
   as `volume` does today (`desktop.js:96`); `last-wallpaper.json` stores the
   engine scale, the gallery library stores the app scale.
5. **Persistence discipline.** Only live properties are written to disk
   (`wallpaperSettings.js` `LIVE_PATHS` + `isLiveProperty`). The registry's
   other ~130 keys stay dead — do not surface them, do not "fix" them.
6. **Close behaviour default is unchanged.** Today close hides + destroys the
   window and the app lives in the tray (the P0 VRAM fix). That must remain the
   default; the new toggle only lets the user choose "quit on close".
7. **Palette discipline.** New CSS uses the `--surface-*` / `--stroke-*` /
   `--text-*` tokens only; no raw colour literals (the `check:palette` guard
   fails the build on them). No glows; champagne only in outlines and the
   app title.
8. **Scrollbars stay hidden** app-wide, and every new scroll container still
   scrolls.
9. **One change per commit**, each commit green on its own gate. The working
   tree carries earlier uncommitted session work — stage only the paths a task
   names, never `git add -A`.

**Verification gates per task**

| Gate | Command |
|---|---|
| Pure-module tests | `node --test app/electron/window-bounds.test.cjs app/src/settings/statusFormat.test.js app/src/wallpapers/wallpaperSettings.test.js` |
| Main-process syntax | `node --check app/electron/<file>.cjs` |
| App build | `pnpm --dir app build` |
| Lint | `pnpm --dir app lint` |
| Palette | `pnpm --dir app check:palette` |
| Live | app running with `--remote-debugging-port=9222`, CDP over the local helper scripts |

**Test-harness reality (read before writing a test).** This repo has **no DOM
test runner** — there is no vitest/jest/jsdom in `app/package.json`. The
established convention is that *logic* lives in a pure module with a
`node:test` file next to it (`mediaInfo.cjs` + `mediaInfo.test.cjs`,
`wallpaperSettings.js` + `wallpaperSettings.test.js`), and *markup* is verified
by `pnpm build` + a live CDP check. Every behaviour this plan adds is
therefore written as a pure function first and tested; the JSX around it is
thin.

---

## Review focus (the failure modes this plan must prevent)

1. **A colour slider that moves the wallpaper but not the engine** — the value
   must reach `SET_BRIGHTNESS` *and* survive a restart *and* come back after a
   wallpaper switch. A value that only lives in the store is the exact bug
   class the inspector rebuild just fixed.
2. **A settings page that spawns the wallpaper engine.** Opening Diagnostics
   must not start the runtime (constraint 2) — and must not show stale
   numbers from a previous scene as if they were current.
3. **A window that opens off-screen** after a monitor is unplugged, or a
   window that never remembers where the user put it (constraint: bounds are
   validated against the live display list before use).
4. **"Quit on close" losing the wallpaper state.** Quitting must flush
   `app-state` and the library *before* the runtime is stopped, or the next
   launch re-applies stale settings.
5. **A control that appears for an image wallpaper** but cannot be applied —
   the static adapter rejects colour adjustments, so the group must be gated
   on the same capability list the engine reports.

---

## File structure

| Path | Responsibility |
|---|---|
| `app/src/settings/SettingsPage.jsx` | **New.** The global settings page: Window, Engine, About sections. |
| `app/src/settings/settings.css` | **New.** Page layout styles (tokens only). |
| `app/src/settings/statusFormat.js` | **New, pure.** `formatStatus(payload)` → display rows. No React, no IPC. |
| `app/src/settings/statusFormat.test.js` | **New.** `node:test` coverage for the formatter. |
| `app/electron/window-bounds.cjs` | **New, pure.** `resolveWindowBounds(saved, displays, fallback)` → bounds to open with. |
| `app/electron/window-bounds.test.cjs` | **New.** `node:test` coverage for bounds resolution. |
| `app/src/wallpapers/wallpaperSettings.js` | Extend: three picture fields in the live set. |
| `app/src/wallpapers/wallpaperSettings.test.js` | Extend: the new live paths. |
| `app/src/wallpapers/desktop.js` | Extend: capability list + engine-scale conversion. |
| `app/src/wallpapers/DesktopApply.jsx` | Extend: the live-update trigger key. |
| `app/src/wallpapers/inspector/sections/PictureSection.jsx` | **New.** Brightness / contrast / saturation. |
| `app/src/wallpapers/inspector/Inspector.jsx` | Extend: mount the Picture section. |
| `app/electron/main.cjs` | Extend: three `SET_*` in the capability map, `wallpaper:update`, `wallpaper:video`, `wallpaper:status`, bounds persist/restore, close behaviour. |
| `app/electron/preload.cjs` | Extend: `getStatus` bridge. |
| `app/electron/app-state-manager.cjs` | Extend: default for the bounds-remember flag. |
| `app/src/App.jsx` | Extend: render the Settings page; use `PAGES[tab].sub`. |

---

### Task 1: Picture group — brightness / contrast / saturation

**Files:**
- Modify: `app/src/wallpapers/wallpaperSettings.js` (live paths, sanitize, pick, apply, defaults)
- Modify: `app/src/wallpapers/wallpaperSettings.test.js`
- Modify: `app/src/wallpapers/desktop.js` (capability list, engine-scale conversion)
- Modify: `app/src/wallpapers/DesktopApply.jsx` (live-update trigger)
- Create: `app/src/wallpapers/inspector/sections/PictureSection.jsx`
- Modify: `app/src/wallpapers/inspector/Inspector.jsx` (mount)
- Modify: `app/electron/main.cjs` (`sendIfSupported` map, `wallpaper:update`, `wallpaper:video`, `applyStoredWallpaper`)
- Modify: `app/electron/startup.cjs` (`mergeLiveUpdate` field map)

**Interfaces:**
- Consumes: `updateWallpaperProperty(id, path, value)` (`store.js:121`),
  `DESKTOP_CAPABILITIES` (`desktop.js:31`), `effectiveCapabilities(typeCaps, bridge)`
  (`capabilities.js:7`).
- Produces:
  - `wallpaperSettings.js`: `PICTURE_DEFAULTS` (`{ brightness: 100, contrast: 100, saturation: 100 }`),
    `PICTURE_KEYS` (`['brightness', 'contrast', 'saturation']`).
  - `wallpaperSettings.js`: `isLiveProperty(['appearance', 'brightness']) === true`; the same for
    `contrast` and `saturation`.
  - `desktop.js`: `updateDesktopSettings(w)` sends `opts.brightness` / `opts.contrast` /
    `opts.saturation` on the **engine scale** (0–2) when the capability list contains
    `brightness` / `contrast` / `saturation`.
  - `main.cjs`: `wallpaper:update` accepts `{ brightness, contrast, saturation }` on the
    **engine scale** and forwards them as `SET_BRIGHTNESS` / `SET_CONTRAST` / `SET_SATURATION`.

- [x] **Step 1: Write the failing tests for the three picture fields**

In `app/src/wallpapers/wallpaperSettings.test.js` add:

```js
test('the three picture fields are live, persisted and clamped to 0..200', () => {
  const w = wallpaper()
  w.appearance = { brightness: 150, contrast: 50, saturation: 100, opacity: 80, gamma: 1.4 }

  const settings = pickLiveSettings(w)

  // Only the three live fields travel; the other appearance keys stay dead.
  assert.deepEqual(settings.appearance, { brightness: 150, contrast: 50, saturation: 100 })
  assert.equal(isLiveProperty(['appearance', 'brightness']), true)
  assert.equal(isLiveProperty(['appearance', 'contrast']), true)
  assert.equal(isLiveProperty(['appearance', 'saturation']), true)
  assert.equal(isLiveProperty(['appearance', 'opacity']), false)
})

test('picture values are clamped into 0..200 and bad ones are dropped', () => {
  const clean = sanitizeSettings({
    appearance: { brightness: 250, contrast: -10, saturation: 'bright', opacity: 30 },
  })

  assert.deepEqual(clean.appearance, { brightness: 200, contrast: 0 })
})

test('applySettings restores the picture fields and leaves other appearance keys alone', () => {
  const source = wallpaper()
  source.appearance = { brightness: 100, contrast: 100, saturation: 100, opacity: 100, gamma: 1 }

  const next = applySettings(source, { appearance: { brightness: 175 } })

  assert.equal(next.appearance.brightness, 175)
  assert.equal(next.appearance.contrast, 100)
  assert.equal(next.appearance.opacity, 100)
})
```

- [x] **Step 2: Run the tests and verify they fail**

Run: `node --test app/src/wallpapers/wallpaperSettings.test.js`
Expected: FAIL — `pickLiveSettings` returns no `appearance` key, and
`isLiveProperty(['appearance','brightness'])` returns `false`.

- [x] **Step 3: Implement the picture fields in `wallpaperSettings.js`**

Add `PICTURE_KEYS` and `PICTURE_DEFAULTS`; add the three `appearance.<key>` paths to
`LIVE_PATHS`; in `sanitizeSettings` add an `appearance` branch that keeps only
`PICTURE_KEYS`, each clamped to 0…200, dropping non-numbers; in `pickLiveSettings`
emit `appearance` with the three defaults merged; in `applySettings` merge a
sanitized `appearance` over the wallpaper's own.

- [x] **Step 4: Run the tests and verify they pass**

Run: `node --test app/src/wallpapers/wallpaperSettings.test.js`
Expected: PASS (12 cases).

- [x] **Step 5: Wire the capability list and the engine-scale conversion in `desktop.js`**

Add `'brightness'`, `'contrast'`, `'saturation'` to `DESKTOP_CAPABILITIES.VIDEO`
(they must **not** be added to `IMAGE` — the static adapter rejects them). In
`updateDesktopSettings`, for the VIDEO branch, send
`opts.brightness = w.appearance.brightness / 100` (and the other two) when the
resolved capability list contains the property.

- [x] **Step 6: Wire the three commands in `main.cjs`**

- `sendIfSupported`'s property map: add
  `SET_BRIGHTNESS: 'brightness'`, `SET_CONTRAST: 'contrast'`, `SET_SATURATION: 'saturation'`.
- `wallpaper:update`: destructure `brightness, contrast, saturation` and forward each
  with `sendIfSupported` when not `null`.
- `wallpaper:video`: accept the same three (engine scale) and, after the `LOAD`,
  send each `SET_*` — the same treatment `audioMode` already gets (they are not
  part of `LOAD`, so without this a wallpaper switch would inherit the previous
  file's brightness). Failures are logged, never fatal.
- `applyStoredWallpaper`: after the boot `LOAD`, send the same three from
  `params.brightness` / `params.contrast` / `params.saturation` when present.
- `lastApplied.params`: persist the three so a crash replay restores them.

- [x] **Step 7: Persist the three on a live update**

In `app/electron/startup.cjs` `FIELD_BY_UPDATE_CMD` add
`SET_BRIGHTNESS: 'brightness'`, `SET_CONTRAST: 'contrast'`,
`SET_SATURATION: 'saturation'`, so `mergeLiveUpdate` folds them into
`last-wallpaper.json`.

- [x] **Step 8: Add the picture values to the live-update trigger**

In `app/src/wallpapers/DesktopApply.jsx` add the three values to the
`settingsKey` join, so changing one re-sends the live update.

- [x] **Step 9: Create the Picture section**

`app/src/wallpapers/inspector/sections/PictureSection.jsx`: a `PropertySection`
titled "Picture" with three `PropertySlider`s (0–200, step 5, suffix `%`),
rendered only when `capabilities` contains `brightness`; a caption stating that
100 % is unchanged. Write through
`updateWallpaperProperty(wallpaper.id, ['appearance', key], value)`.

- [x] **Step 10: Mount the Picture section**

In `Inspector.jsx`, render `<PictureSection wallpaper={wallpaper} applied={applied}
capabilities={capabilities} />` after `PlaybackSection`.

- [x] **Step 11: Run the gates**

Run: `node --test app/src/wallpapers/wallpaperSettings.test.js` ·
`node --check app/electron/main.cjs` · `node --check app/electron/startup.cjs` ·
`pnpm --dir app build` · `pnpm --dir app lint`
Expected: all green.

- [x] **Step 12: Live check**

With the app running and a **video** wallpaper applied: open the Inspector,
drag Brightness to 150 %, confirm the app log shows `SET_BRIGHTNESS` applied,
confirm the wallpaper visibly brightens, restart the app, confirm the slider
still reads 150 and the wallpaper is still brightened, then apply a *different*
wallpaper and confirm the brightness did **not** carry over.

- [x] **Step 13: Commit**

```bash
git add app/src/wallpapers/wallpaperSettings.js app/src/wallpapers/wallpaperSettings.test.js \
        app/src/wallpapers/desktop.js app/src/wallpapers/DesktopApply.jsx \
        app/src/wallpapers/inspector/sections/PictureSection.jsx \
        app/src/wallpapers/inspector/Inspector.jsx \
        app/electron/main.cjs app/electron/startup.cjs
git commit -m "Expose the engine's live colour controls as a Picture group"
```

---

### Task 2: The global Settings page, window bounds, and close behaviour

**Files:**
- Create: `app/electron/window-bounds.cjs`
- Create: `app/electron/window-bounds.test.cjs`
- Create: `app/src/settings/SettingsPage.jsx`
- Create: `app/src/settings/settings.css`
- Modify: `app/electron/main.cjs` (bounds persist/restore, close behaviour)
- Modify: `app/electron/app-state-manager.cjs` (default for the remember flag)
- Modify: `app/src/App.jsx` (render the page, use `PAGES[tab].sub`)
- Modify: `app/src/components/SidebarLayout.css` (page styles) — or `settings.css`

**Interfaces:**
- Consumes: `app-state:get` / `app-state:set` IPC (`main.cjs:780,789`),
  `useAppState(keyPath, defaultValue)` (`hooks/useAppState.jsx:6`).
- Produces:
  - `window-bounds.cjs`: `resolveWindowBounds(saved, displays, fallback)` →
    `{ x, y, width, height } | null`. `saved` is `{x,y,width,height}` or `null`;
    `displays` is `[{ bounds: {x,y,width,height} }]`; `fallback` is
    `{ width, height }`. Returns `null` when there is nothing to restore.
  - `main.cjs`: the window remembers its bounds under `window` in `app-state.json`,
    and `tray.closeToTray` (`false` = quit on close) is honoured.

- [x] **Step 1: Write the failing tests for bounds resolution**

`app/electron/window-bounds.test.cjs`:

```js
const test = require('node:test')
const assert = require('node:assert/strict')
const { resolveWindowBounds } = require('./window-bounds.cjs')

const display = (x, y, width, height) => ({ bounds: { x, y, width, height } })

test('nothing saved means nothing to restore', () => {
  assert.equal(resolveWindowBounds(null, [display(0, 0, 1920, 1080)], { width: 1280, height: 800 }), null)
})

test('a saved window on a live display is restored as-is', () => {
  const saved = { x: 100, y: 120, width: 1000, height: 700 }
  const out = resolveWindowBounds(saved, [display(0, 0, 1920, 1080)], { width: 1280, height: 800 })
  assert.deepEqual(out, saved)
})

test('a saved window on a disconnected monitor falls back to null', () => {
  const saved = { x: 4000, y: 200, width: 1000, height: 700 }
  assert.equal(resolveWindowBounds(saved, [display(0, 0, 1920, 1080)], { width: 1280, height: 800 }), null)
})

test('a saved window that only partly overlaps a display is still restored', () => {
  const saved = { x: 1800, y: 1000, width: 400, height: 300 }
  const out = resolveWindowBounds(saved, [display(0, 0, 1920, 1080)], { width: 1280, height: 800 })
  assert.deepEqual(out, saved)
})

test('a saved size below the minimum is not restored', () => {
  const saved = { x: 10, y: 10, width: 10, height: 10 }
  assert.equal(resolveWindowBounds(saved, [display(0, 0, 1920, 1080)], { width: 1280, height: 800, minWidth: 960, minHeight: 640 }), null)
})
```

- [x] **Step 2: Run the tests and verify they fail**

Run: `node --test app/electron/window-bounds.test.cjs`
Expected: FAIL — `Cannot find module './window-bounds.cjs'`.

- [x] **Step 3: Implement `window-bounds.cjs`**

`resolveWindowBounds(saved, displays, fallback)`:
returns `null` if `saved` is missing/incomplete; returns `null` if `saved.width`
or `saved.height` is below `fallback.minWidth` / `fallback.minHeight` (when
given); returns `null` if the saved rectangle does not intersect any display's
`bounds`; otherwise returns `{ x, y, width, height }` unchanged.

- [x] **Step 4: Run the tests and verify they pass**

Run: `node --test app/electron/window-bounds.test.cjs`
Expected: PASS (5 cases).

- [x] **Step 5: Persist and restore the window bounds in `main.cjs`**

- Import `screen` from `electron` and `resolveWindowBounds` from
  `window-bounds.cjs`.
- In `createWindow`, read `appState.get('window')` and, when
  `resolveWindowBounds` returns bounds, pass `x`/`y`/`width`/`height` to the
  `BrowserWindow` constructor instead of the hard-coded 1280×800.
- Listen to `resize` and `move` on the window; debounce 400 ms and write
  `{ x, y, width, height }` to `appState.set('window', …)`. Write the *normal*
  bounds, not the maximized ones.
- Gate all of this on a persisted flag `ui.rememberWindowBounds` (default
  `true`) so the user can turn it off; add the default to
  `app-state-manager.cjs` `defaultState().ui`.

- [x] **Step 6: Honour the close behaviour**

In the window `close` handler (`main.cjs:361`), read
`appState.get('tray.closeToTray')`. When it is `false`, call the existing
`quitApp()` path (which flushes `app-state` and stops the runtime) instead of
hiding the window. When it is `true` (the default) the current behaviour —
hide, then destroy — is unchanged.

- [x] **Step 7: Create the Settings page**

`app/src/settings/SettingsPage.jsx`, rendered inside the app's content column.
Sections, in order:

1. **Window** — "Remember window size and position" (toggle → `ui.rememberWindowBounds`),
   "On close" (dropdown: `Keep running in the tray` / `Quit REX` → `tray.closeToTray`).
2. **Engine** — the Diagnostics block from Task 3 (placeholder here if Task 3
   has not landed yet).
3. **About** — the app version and the runtime's status line.

Use the shared `PropertySection` / `PropertyGroup` / `PropertyRow` /
`PropertyToggle` / `PropertyDropdown` components and the shared
`useAppState` hook, so the page and the playlist dialog look like one product.
Wrap the page in `TooltipProvider` (Radix's `Tooltip` requires it, and
`PropertyRow` uses it).

- [x] **Step 8: Render the page and wire the page subtitle**

In `App.jsx`, render `<SettingsPage />` when `tab === 'Settings'`, and show
`PAGES[tab].sub` in the page head (the `PAGES` table exists but is never read
today — this is its first use).

- [x] **Step 9: Style the page**

`app/src/settings/settings.css`: a two-column section grid that collapses to
one column under 900 px, using only `--surface-content`, `--stroke-default`,
`--stroke-subtle`, `--text-primary`, `--text-secondary`, `--text-tertiary` and
the `--fill-*` tokens. No raw colour literals, no glows. Import it from
`SettingsPage.jsx`.

- [x] **Step 10: Run the gates**

Run: `node --test app/electron/window-bounds.test.cjs` ·
`node --check app/electron/main.cjs` · `pnpm --dir app build` ·
`pnpm --dir app lint` · `pnpm --dir app check:palette`
Expected: all green.

- [x] **Step 11: Live check**

Launch the app; move and resize the window; close it; reopen it — the window
returns to the same place and size. Unplug/move a monitor scenario is covered
by the unit test, not by unplugging hardware. Toggle "On close → Quit REX",
close the window, and confirm the process exits **and** that the next launch
restores the last wallpaper and its settings.

- [x] **Step 12: Commit**

```bash
git add app/electron/window-bounds.cjs app/electron/window-bounds.test.cjs \
        app/electron/main.cjs app/electron/app-state-manager.cjs \
        app/src/settings/SettingsPage.jsx app/src/settings/settings.css \
        app/src/App.jsx
git commit -m "Add the global Settings page with window memory and close behaviour"
```

---

### Task 3: Diagnostics — the engine's own telemetry

**Files:**
- Create: `app/src/settings/statusFormat.js`
- Create: `app/src/settings/statusFormat.test.js`
- Modify: `app/electron/main.cjs` (`wallpaper:status` IPC)
- Modify: `app/electron/preload.cjs` (`getStatus` bridge)
- Modify: `app/src/settings/SettingsPage.jsx` (Engine section)

**Interfaces:**
- Consumes: `runtime.proc` (main process), `runtime.send('GET_STATUS')`.
- Produces:
  - `main.cjs`: `wallpaper:status` → `{ available: boolean, reason?: string, data?: object }`.
    `available` is `false` with `reason: 'runtime-stopped'` when no runtime
    process exists — **without sending a command** (constraint 2).
  - `preload.cjs`: `rexDesktop.getStatus()` → the same object.
  - `statusFormat.js`: `formatStatus(payload)` → `{ rows: [{ label, value, tone? }], state: string }`.

- [x] **Step 1: Write the failing tests for the formatter**

`app/src/settings/statusFormat.test.js`:

```js
import test from 'node:test'
import assert from 'node:assert/strict'
import { formatStatus } from './statusFormat.js'

test('an empty payload yields no rows and an unknown state', () => {
  const out = formatStatus(null)
  assert.deepEqual(out.rows, [])
  assert.equal(out.state, 'unknown')
})

test('the live scene and the measured counters become rows', () => {
  const out = formatStatus({
    state: 2,
    playing: true,
    renderer: 'video',
    privateBytes: 314572800,
    position: 12.5,
    duration: 60,
    scene: { path: 'D:/a.mp4', fillMode: 'fill', muted: false, volume: 0.8, speed: 1 },
    telemetry: {
      renderFps: 59.8, decoderFps: 24, schedulerDrops: 3, avSyncDriftMs: 1.2,
      gpuDedicated: 268435456, gpuShared: 134217728, gpuBudget: 1073741824,
      hdrActive: false, displayMaxNits: 0, sourceFps: 23.976, displayHz: 60,
    },
  })

  const labels = out.rows.map((r) => r.label)
  assert.ok(labels.includes('Frame rate'))
  assert.ok(labels.includes('VRAM (dedicated)'))
  assert.ok(labels.includes('Dropped frames'))
  assert.ok(labels.includes('A/V drift'))
  assert.ok(labels.includes('HDR'))
  assert.equal(out.state, 'playing')
})

test('a missing telemetry block does not throw and marks the state unknown', () => {
  const out = formatStatus({ state: 0, playing: false })
  assert.equal(out.state, 'stopped')
  assert.equal(out.rows.length, 0)
})
```

- [x] **Step 2: Run the test and verify it fails**

Run: `node --test app/src/settings/statusFormat.test.js`
Expected: FAIL — module not found.

- [x] **Step 3: Implement `statusFormat.js`**

`formatStatus(payload)` returns `{ state, rows }`:
`state` is `'playing'` when `payload.playing`, `'stopped'` when `state === 0`,
`'paused'` when `state === 1`, else `'unknown'`.
Rows are built from the payload in this order, each skipped when its value is
missing: Frame rate (`renderFps`), Source rate (`sourceFps`), Display (`displayHz`),
Dropped frames (`schedulerDrops`), A/V drift (`avSyncDriftMs`), VRAM dedicated
(`gpuDedicated`), VRAM shared (`gpuShared`), VRAM budget (`gpuBudget`),
Process memory (`privateBytes`), HDR (`hdrActive` → "Active"/"Off"), File
(`scene.path`). Byte values are formatted as MiB; milliseconds to 1 decimal.

- [x] **Step 4: Run the test and verify it passes**

Run: `node --test app/src/settings/statusFormat.test.js`
Expected: PASS (3 cases).

- [x] **Step 5: Add the `wallpaper:status` IPC in `main.cjs`**

```js
ipcMain.handle('wallpaper:status', async () => {
  // Never spawn a runtime to answer a question: the panel must not start the
  // engine (and its VRAM) just because the user opened the page.
  if (!runtime.proc) return { available: false, reason: 'runtime-stopped' }
  try {
    const data = await runtime.send('GET_STATUS', {}, 5000)
    if (!data) return { available: false, reason: 'no-answer' }
    return { available: true, data }
  } catch (err) {
    logLine('wallpaper:status ERROR', err.message)
    return { available: false, reason: 'error' }
  }
})
```

- [x] **Step 6: Expose it to the renderer**

In `preload.cjs` add `getStatus: () => ipcRenderer.invoke('wallpaper:status')`.

- [x] **Step 7: Render the Engine section**

In `SettingsPage.jsx`, add an "Engine" section that calls `getStatus()` on
mount and every 2 s while the page is visible (clear the interval on unmount),
formats the answer with `formatStatus`, and renders the rows. When
`available` is `false`, say why ("The wallpaper engine is not running") instead
of showing zeros.

- [x] **Step 8: Run the gates**

Run: `node --test app/src/settings/statusFormat.test.js` ·
`node --check app/electron/main.cjs` · `pnpm --dir app build` ·
`pnpm --dir app lint` · `pnpm --dir app check:palette`
Expected: all green.

- [x] **Step 9: Live check**

With a video wallpaper applied, open Settings → Engine and confirm the live
frame rate, VRAM and drop counters appear. Stop the wallpaper (or set an image
wallpaper, which reclaims the runtime) and confirm the panel says the engine
is not running and that **no runtime process was started** by opening the page
(check the process list before and after).

- [x] **Step 10: Commit**

```bash
git add app/src/settings/statusFormat.js app/src/settings/statusFormat.test.js \
        app/src/settings/SettingsPage.jsx app/electron/main.cjs app/electron/preload.cjs
git commit -m "Surface the engine's own telemetry in Settings"
```

---

## Out of scope (Tier B and C, deliberately)

- **Live pause-on-fullscreen** (B1), **live frame-rate cap** (B2), **live
  rotation/flip** (B3) — all need an engine change, and `rex-core` carries
  uncommitted WIP. Tier A touches no engine.
- **Per-app rules** (C1, = TODO P3-1), **multi-monitor profiles** (C2, = P3-2),
  **screensaver** (C3), **HDR policy** (C4), **suspend policy** (C5).
- The dead surface listed in `research-settings.md` §4 is **not** surfaced:
  the eight unimported Inspector sections, the seven no-op `multiMonitor`
  toggles, the playlist filters that read placeholder data, `automation.*`
  (simulated signals), and `theme.*` (contradicts the single-palette decision).
