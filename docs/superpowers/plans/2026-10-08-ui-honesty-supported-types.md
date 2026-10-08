# UI Honesty: Supported Wallpaper Types Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Users can no longer select or apply wallpaper types the engine cannot display (WEB, SCENE, SHADER, AUDIO_VISUALIZER); those types show "Coming later" and applying one never stops the current wallpaper.

**Architecture:** Single source of truth (`SUPPORTED_WALLPAPER_TYPES` + `isSupportedWallpaperType()` in `registry.js`) consumed at the three UI chokepoints — picker filter, tile click, apply routing — plus a guard in `loadWallpaperFile`. No engine, protocol, or dependency changes.

**Tech Stack:** React (Vite), plain JS modules, oxlint, `vite build`.

**Spec:** User strategy decision (v1 focuses Image/Video; hide the rest until demand) + TODO.md backlog item "UI advertises unimplemented wallpaper types". Verified facts: `desktop.js:67` stops the desktop for any non-IMAGE/VIDEO type; `DESKTOP_CAPABILITIES` already lists the four as `[]`; gallery picker `WallpaperGallery.jsx:308` accepts their extensions; tiles render for every imported file (`WallpaperGallery.jsx:472-530`); no JS test runner exists in `app/`.

## Global Constraints

- No changes under `rex-core/`, no protocol changes, no new npm dependencies.
- `oxlint` clean (`pnpm lint`) and `vite build` green after every task.
- Keep the `image/*` / `video/*` MIME wildcards in the picker (OS coverage for bmp/ico/mkv/mov).
- No toast/notification system — reuse tile badge + `title` text + `console.info` (nothing else exists).
- Testing exception (no JS runner in repo; flagged for approval): pure-logic checks run as an uncommitted `node -e` one-liner against `registry.js` (package `"type": "module"`, lucide-react resolvable); UI behavior is verified by the manual matrix in Task 6 plus lint+build. Adding vitest is out of scope.

## Review Focus

- Unknown extensions fall back to `?? 'IMAGE'` (`WallpaperGallery.jsx:474`, `store.js:142`) — e.g. `.txt` imports as IMAGE and flows to the OS wallpaper setter, which warns on failure. Out of scope, but a reviewer should confirm that path still only warns.
- The `C:\REX\` seed guard (`desktop.js:43`) is untouched — seed records still never apply.
- `main.cjs` IMAGE/VIDEO IPC gates are untouched — image/video apply paths must behave byte-identically.
- Live `muted` toggle still missing (initial-only) — unchanged by this plan, do not fix here.
- Drag-and-drop (if added later) bypasses the `accept` filter — Task 3+4 guards cover it anyway since they don't rely on the picker.

---

### Task 1: Support source of truth in `registry.js`

**Files:**
- Modify: `app/src/wallpapers/registry.js:21-34`
- Test: uncommitted `node -e` one-liner (see steps)

**Interfaces:**
- Consumes: nothing new (`TYPE_META`, `WALLPAPER_TYPES` stay as-is for labels/classification).
- Produces: `SUPPORTED_WALLPAPER_TYPES`, `isSupportedWallpaperType(type) -> boolean` (Tasks 2–5 import these); `TYPE_META[type].comingLater -> boolean` for badge text.

- [ ] **Step 1: Prove the gap with a failing check**

```bash
node --input-type=module -e "import('./app/src/wallpapers/registry.js').then(m => { if (typeof m.isSupportedWallpaperType !== 'function') throw new Error('missing'); console.log(m.isSupportedWallpaperType('WEB'), m.isSupportedWallpaperType('VIDEO')) })"
```

Expected: throws `missing` (function does not exist).

- [ ] **Step 2: Implement in `app/src/wallpapers/registry.js`**

```js
export const SUPPORTED_WALLPAPER_TYPES = ['IMAGE', 'VIDEO']
export function isSupportedWallpaperType(type) { return SUPPORTED_WALLPAPER_TYPES.includes(type) }
```

plus `comingLater: true` on the `WEB`, `SCENE`, `SHADER`, `AUDIO_VISUALIZER` entries of `TYPE_META` (exact text `'Coming later'` is composed in Task 3 from this flag).

- [ ] **Step 3: Re-run the Step 1 one-liner**

Expected: prints `false true`, exit 0.

- [ ] **Step 4: Commit**

```bash
git add app/src/wallpapers/registry.js
git commit -m "feat(ui): supported wallpaper types source of truth"
```

### Task 2: Picker accepts only supported types

**Files:**
- Modify: `app/src/wallpapers/WallpaperGallery.jsx:308`

**Interfaces:**
- Consumes: `SUPPORTED_WALLPAPER_TYPES` from Task 1 (import next to the existing `registry` import at line 12) — derive the accept string from the IMAGE+VIDEO extension sets so picker and `detectFileType` cannot drift; keep `image/*,video/*` wildcards.

- [ ] **Step 1: Change the `accept` attribute to the derived supported-only value**

No automated test exists for JSX (see Global Constraints); verification is Task 6 matrix row 1.

- [ ] **Step 2: Run `pnpm lint`**

Expected: clean.

- [ ] **Step 3: Commit**

```bash
git add app/src/wallpapers/WallpaperGallery.jsx
git commit -m "feat(ui): file picker accepts only image/video"
```

### Task 3: Tiles badge unsupported types and never apply them

**Files:**
- Modify: `app/src/wallpapers/WallpaperGallery.jsx:218-222` (`handleOpen`), `:214` (`handleFiles` auto-load), `:472-530` (tile render)

**Interfaces:**
- Consumes: `isSupportedWallpaperType()`, `TYPE_META[type].comingLater` from Task 1.

- [ ] **Step 1: Guard `handleOpen` and the `handleFiles` auto-load**

```js
if (!isSupportedWallpaperType(detectFileType(name) ?? 'IMAGE')) { console.info(...); return }
```

i.e. return before `loadWallpaperFile` so the current desktop wallpaper is never stopped. Badge render: `{meta.comingLater && <span className="gallery-tile-coming">Coming later</span>}` plus `title` suffix.

- [ ] **Step 2: Run `pnpm lint`**

Expected: clean.

- [ ] **Step 3: Commit**

```bash
git add app/src/wallpapers/WallpaperGallery.jsx
git commit -m "feat(ui): unsupported tiles labeled, clicks do not apply"
```

### Task 4: `loadWallpaperFile` rejects unsupported types

**Files:**
- Modify: `app/src/wallpapers/store.js:139-166`

**Interfaces:**
- Consumes: `isSupportedWallpaperType()` from Task 1.

- [ ] **Step 1: Return `null` for unsupported types right after the `detectFileType` line (~142), with a `console.info`**

Callers (`handleOpen`, `handleFiles`) ignore the return value except null-check at `store.js:144`, so no caller changes.

- [ ] **Step 2: Run `pnpm lint`**

Expected: clean.

- [ ] **Step 3: Commit**

```bash
git add app/src/wallpapers/store.js
git commit -m "feat(ui): loadWallpaperFile rejects unsupported types"
```

### Task 5: Apply path warns instead of stopping

**Files:**
- Modify: `app/src/wallpapers/desktop.js:37-68`

**Interfaces:**
- Consumes: `isSupportedWallpaperType()` from Task 1. `DESKTOP_CAPABILITIES` needs no change (unsupported already `[]`).

- [ ] **Step 1: Replace the fallthrough `setDesktopVideo({ path: null })` (line 67) with a `console.warn` + `return` for unsupported types**

The explicit null-path STOP (`!w.filePath`, lines 39-41) and the IMAGE/VIDEO branches stay byte-identical.

- [ ] **Step 2: Run `pnpm lint`**

Expected: clean.

- [ ] **Step 3: Commit**

```bash
git add app/src/wallpapers/desktop.js
git commit -m "feat(ui): unsupported apply warns instead of stopping desktop"
```

### Task 6: Verify, record, commit docs

**Files:**
- Modify: `TODO.md` (check off the UI-honesty backlog item), `everything.md` (append entry per AGENTS.md)

- [ ] **Step 1: Run the full verification**

```bash
pnpm lint
pnpm --dir app exec vite build
```

Expected: both green. Then the manual matrix (Electron dev, `pnpm dev`): (1) picker dialog shows no html/glsl/glb/mp3; (2) an unsupported tile (pre-existing import) shows "Coming later" and clicking it leaves the current wallpaper running; (3) applying an image then a video still works, settings flow unchanged; (4) devtools console shows `info`/`warn`, never a STOP, for unsupported attempts; (5) importing `notes.txt` still classifies IMAGE and only warns from the OS setter (fallback behavior unchanged).

- [ ] **Step 2: Update `TODO.md` + `everything.md` and commit**

```bash
git add TODO.md everything.md
git commit -m "docs: UI-honesty fix recorded"
```
