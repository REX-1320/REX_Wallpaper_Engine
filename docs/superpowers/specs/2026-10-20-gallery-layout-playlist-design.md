# REX Gallery Layout + Playlist Dock — Design Specification

**Status:** approved in conversation (2026-10-20) · **Partially implemented** (2026-10-20) — see
§11 *Implementation status* for what shipped and what is still held.
**Related:** [`2026-10-20-performance-modes-design.md`](2026-10-20-performance-modes-design.md) (the planned
Performance page lives in this shell and must land on top of it).

> Every `file:line` below was verified against the tree on 2026-10-20.

---

## 1. Intent

The wallpaper app's Wallpapers page gets the two layout behaviours people already know from VS Code:

1. **A draggable divider** between the gallery and the inspector, with the width remembered.
2. **A bottom dock panel** (VS Code's terminal) holding the **playlist** — a horizontal strip of the playlist's
   files — that is resizable, collapsible, and global across pages.

On top of that, four behaviour changes the feature exposes:

- **Multi-select in the gallery** with Windows-Explorer semantics (rubber-band drag, Ctrl+click, Shift+click).
- **Drag a file from the gallery into the playlist** — playlists become real containers of files, a file may live
  in several playlists at once, and **a playlist's members become the pool the playlist engine plays**.
- **A heart on each tile** that favorites a file, plus a Favorites filter in the gallery toolbar.
- **The Inspector shows the selected file's thumbnail.**

Everything here is wallpaper-app chrome. It does not touch the native engine, the runtime protocol, or the
wallpaper path itself.

---

## 2. Layout model

### 2.1 The shell

The nav column (240 px open / 51 px collapsed) stays as it is. Everything else moves into a **content column**
that is a flex box, so the dock can sit at the bottom of it and the page above fills whatever is left.

```
┌────────┬──────────────────────────────────────────────┐
│ nav    │  Wallpapers page                             │
│ 240px  │  ┌──────────────┬┃┌──────────────────────┐  │
│ fixed  │  │   gallery    │┃│     inspector        │  │
│        │  │  (resizable) │┃│  (thumbnail on top)  │  │
│        │  └──────────────┴┃└──────────────────────┘  │
│        ├──────────────────────────────────────────────┤
│        │ ▤ Nature ▾  (config)              -  []  X  │  ← drag top edge
│        │ [chip][chip][chip][chip][chip]…             │  ← the strip
└────────┴──────────────────────────────────────────────┘
```

New DOM shape inside `.window` (`SidebarLayout.jsx`):

```
.sidebar                       (unchanged; width animated by motion)
.content-col                   position:absolute; top:140px; right:0; bottom:0;
  ├ .page-area                flex:1 1 auto; min-height:0; overflow-y:auto; padding:24px
  └ .dock                     flex:0 0 var(--dock-h)
```

`.window-content` (today `position:absolute; top:140px; right:0; bottom:0` + animated `left`) becomes `.page-area`
inside `.content-col`; the animated `left` moves to `.content-col`.

**One source of truth for the nav width.** Today the two numbers are hand-synced — `SidebarLayout.jsx:138`
(`width: open ? 240 : 51`) and `SidebarLayout.jsx:182` (`left: open ? 260 : 71`). They become constants
(`SIDEBAR_W = 240`, `SIDEBAR_CLOSED_W = 51`, `CONTENT_GAP = 20`) with `left = width + CONTENT_GAP` derived from
them, so the gap can never drift.

### 2.2 The page must size to its box, not the viewport

`.wallpaper-grid` is `height: calc(100vh - 252px)` (`SidebarLayout.css:454`) — a viewport constant that knows
nothing about the dock. With a dock, the grid fills the page area instead. The flex chain has **three** links
and all three must be flex children, or `flex: 1` resolves to `auto` and the grid collapses:

```
.page-area        flex:1 1 auto; min-height:0; display:flex; flex-direction:column
  └ .content-section   flex:1; min-height:0; display:flex; flex-direction:column   (App.jsx:13)
      └ .tab-panel.plain  flex:1; min-height:0; display:flex; flex-direction:column (App.jsx:22)
          └ .wallpaper-grid   flex:1; min-height:0                              (App.jsx:23)
```

`height: calc(100vh - 252px)` is **deleted**.

### 2.3 The split

`.wallpaper-grid` is `display:grid; grid-template-columns: 1fr 1fr` (`SidebarLayout.css:448-450`) with exactly
two children — `.wallpaper-cube.w1` (gallery) and `.wallpaper-cube.w2` (inspector) — built in `App.jsx:24-29`.
It becomes three columns:

```
grid-template-columns: var(--gallery-w) 8px minmax(320px, 1fr);
```

- The **middle column is a new child of `.wallpaper-grid`** — the split handle, inserted in `App.jsx`. It is an
  18 px hit area (the panels' old 18 px gutter, now with a hairline on hover), `cursor: col-resize`.
- The inspector is `minmax(320px, 1fr)` so it can never be squeezed out of existence; the gallery column has a
  floor of 280 px (a drag below that clamps).
- The two `.wallpaper-cube` children carry panel chrome (borders, padding, black inspector background) — the
  handle is a sibling of them, not a child, so their `overflow` and padding are unaffected.

### 2.4 The dock

- Height: `var(--dock-h)`, default **220 px**, resizable by dragging the dock's top edge (6 px hit area,
  `cursor: row-resize`). Clamped to 120 px … 70 % of the window height.
- **Minimize `-`**: collapse to a 36 px header strip (header stays, strip hides).
- **Maximize `[]`**: the dock takes the whole page area (the page above collapses) — VS Code's "maximize panel".
- **Close `X`**: hide the dock; the page area takes the full height. A toolbar button brings it back.
- The dock is **global**: it stays docked at the bottom on every page (Wallpapers, Settings, About).

---

## 3. Data model

### 3.1 Playlists own their members — and the engine plays them

A playlist today is a **rule set** over every wallpaper, and it is *live*: `fireChange` picks the next file and
applies it to the desktop (`playlist/engine.js:516-536`, `applyWallpaper(next.id)` at `:529`). Its pool is
`filterPool(settings, poolWallpapers())` — **all** store wallpapers narrowed by the playlist's filters
(`engine.js:353-401`). A playlist's identity is found by **name**: `activePlaylistId()` matches
`selected.playlist.currentPlaylist` against `playlist.name` (`engine.js:358-363`).

The new model:

```
playlists = [
  { id: 'pl-1', name: 'Favorites', icon: 'heart', members: [...], virtual: true },
  { id: 'pl-2', name: 'Nature',    icon: 'mountain', members: ['C:\\…\\a.mp4', …] },
]
```

- `members` is an **ordered list of file paths**. Order = the strip's order; dragging a chip reorders it.
- Membership is keyed by **path**, never by the store's `file-N` id: those ids come from a counter
  (`store.js:12`, used at `store.js:160`) and are reassigned on every restart, so an id-keyed list would scramble.
- A file may be in any number of playlists. Adding to one never removes it from another.
- The **`Favorites` playlist is virtual**: its membership is *derived* from the favorite flag (§3.2), not stored.
  The heart on a tile is therefore "add/remove from Favorites" in effect, without a second code path.
- **The engine's pool becomes the membership.** `filterPool` is given the active playlist's members and narrows
  to them before applying the existing filters. A playlist with **no** members keeps today's behaviour
  (all wallpapers ∩ filters) so an empty playlist never breaks rotation. The strip and the engine then tell the
  same truth.

**`wallpaper.playlist.currentPlaylist` stays a name and is not renamed.** The engine reads it by name in at least
five places (`engine.js:362,519-520,757`, `PlaylistEngineDialog.jsx:105,141`, and four name-based spots in
`WallpaperGallery.jsx:108,601,623,848`) — renaming it to an id would be churn with no visible gain. It becomes
"the playlist whose rules govern this file", i.e. the **active** playlist, and the dock's chip context menu sets
it ("Set as active playlist"). It is *not* the file's only membership.

**Built-in playlists keep their ids.** `PLAYLISTS = ['None','Favorites','Nature','Gaming','Mixed']`
(`registry.js:112`) maps to `pl-0…pl-4`, and the engine's `settingsByPlaylist` is keyed by those ids — changing
them would orphan every saved rule set. `pl-0` ("None") stays as the "no active playlist" sentinel but is **not
shown in the picker**. `pl-1` (Favorites) is the virtual one and is injected at runtime, not persisted.

### 3.2 Favorites — one durable home, one in-memory mirror

There are **three** places a "favorite" currently lives:

| Home | Shape | Written by | Read by |
|---|---|---|---|
| `general.favorite` (`registry.js:175`) | on the store record | — | the engine's filters/weighting/order (`engine.js:370,398,422,448`) |
| `info.favorite` (`registry.js:386`) | on the store record | `inspector/sections/InfoSection.jsx:55` | — (and **InfoSection is not mounted anywhere**) |
| the persisted gallery library | per **path**, `favorite: false` (`wallpaper-library.cjs:132`) | `library.update()` (`:154`), exposed as `window.rexDesktop.gallery.update` (`preload.cjs:41`) | `library.list({favorite:true})` (`:173`) |

Resolution:

- The **persisted gallery library is the durable home** — it is already path-keyed, already persisted, already has
  an update IPC and a favorite filter. No new persistence channel is invented for it.
- `general.favorite` becomes the **in-memory mirror** the engine reads, hydrated from the library when a store
  record is created, and written back by the heart.
- `info.favorite` stays dead (its only writer, `InfoSection`, is not mounted — see §6).
- The store's default becomes `false` (`registry.js:175`), so a record that has not been hydrated is not a
  favorite. Today every wallpaper is one, which makes the engine's "Favorites Only" filter a no-op.

### 3.3 Selection

The store's single `selectedId` cannot express Explorer selection. The store gains a **path-keyed** selection:

```
selectedPaths: string[]     // ordered, path-keyed — stable across restarts
selectedId: string | null   // the focused file's store entry (the Inspector's subject)
```

- `selectWallpaper(id)` keeps its meaning for the Inspector and also sets `selectedPaths` to that one file's path.
- New: `setSelection(paths, focusedId)`, `toggleSelection(path)`, `extendSelection(paths)`, `clearSelection()`.
- `loadWallpaperFile` sets `selectedId` **directly** (`store.js:188`) and must be updated too, or every load path
  (gallery select, add-to-playlist, the dock applying a chip) desyncs `selectedPaths`.
- The Inspector's *Deselect* button clears both.

The gallery's file records and the store's wallpaper records are two parallel lists joined by **file name**
(`WallpaperGallery.jsx:382,657`); the path is the join key for everything new, with the name as the fallback for
entries that have no path.

### 3.4 Thumbnails

Posters live only in the gallery's local file records (`record.thumbDataUrl` / `record.thumbPath`,
`gallery.js:73-74`). The dock is global and the Inspector is a sibling of the gallery, so posters need one shared
cache keyed by path:

```
app/src/wallpapers/thumbCache.js
  getThumb(key)      → { dataUrl, posterPath } | null
  setThumb(key, v)
  ensureThumb(key)   → decode a poster for a key not cached yet
```

- The key is the file **path**, with the file **name** as the fallback key (dev builds have no paths — §8).
- The gallery populates the cache at import (`createThumbnail(f, 480, thumbUrl)` → `gallery.js:56`) and at restore
  (`createThumbnailFromUrl(r.thumbUrl, r.isVideo)` → `WallpaperGallery.jsx:228`; the restore path deliberately
  skips the thumbnail inside `buildFileRecord` and decodes it separately, so **both** paths must feed the cache).
- `ensureThumb` decodes through the same two existing paths; no new `file://` loading route is introduced
  (Electron blocks `file://` subresources from the dev-server origin, so the existing decode path is the only one
  that works in dev).
- The **Inspector pulls** the poster from the cache by the record's `filePath` (falling back to `info.source`).
  Nothing is pushed into the store record — the store is rebuilt every session, so a field on it is not a cache.

### 3.5 The dock is backed by the library

The dock renders outside the gallery, so it cannot read the gallery's local `files` state. It resolves what it
needs from the **persisted library** (`window.rexDesktop.gallery.list()` → `{path, title, thumbnail, favorite}`):
the chip's **name** (`entry.title`, i.e. `path.basename` — `wallpaper-library.cjs:128`) and its poster
(`entry.thumbnail`), with `path.basename` and the type icon as fallbacks. Membership is path-keyed, so the dock
and the gallery agree on identity without sharing state.

Clicking a chip focuses a file that may have **no store record yet** (records are created lazily, on
select/apply). The store therefore grows `ensureWallpaperForPath(path)` — the gallery's `ensureLoaded`
(`WallpaperGallery.jsx:381-391`) logic, moved into the store so the dock can use it too.

---

## 4. Interactions

### 4.1 Gallery selection (Explorer semantics)

| Gesture | Result |
|---|---|
| Click a tile | select it (replaces the selection), focus it |
| Ctrl/Cmd + click | toggle that file in/out of the selection, focus it |
| Shift + click | select the range between the focused file and the clicked one, over the **currently sorted** list |
| Click empty gallery space | clear the selection |
| Drag on empty gallery space | rubber-band select (replaces; adds when Ctrl is held) |
| Double click | apply to the desktop (unchanged — `WallpaperGallery.jsx:674`) |
| Drag on a tile | drag the file(s) into a playlist (§4.3) |

**Marquee geometry.** The scrolling container is **`.gallery-grid`** (`SidebarLayout.css:1534-1545`,
`overflow-y:auto`) — *not* `.wallpaper-grid`. The marquee rectangle is therefore computed in `.gallery-grid`
content coordinates:

```
rect.top    = clientY - gridRect.top + grid.scrollTop
rect.left   = clientX - gridRect.left + grid.scrollLeft
```

`.gallery-grid` needs `position: relative`, and the overlay must live in a `position: relative` inner wrapper
(the scrolling content), not be a child of the animated children — `motion.button` tiles carry a `layout` prop
(`WallpaperGallery.jsx:660`) and would measure mid-animation.

**Hit test.** The gallery is a plain CSS grid with **every tile mounted** — the windowed-grid component
`GalleryGrid.jsx` is imported nowhere — so the hit test is a rectangle intersection against each tile's
`getBoundingClientRect()`, throttled to one test per animation frame.

**Dragging a tile** is an HTML5 drag (`draggable` on the tile). The heart and the rename input inside a tile opt
out (`draggable={false}` + `stopPropagation`) so they never start a drag.

### 4.2 The heart

A 20 px heart at the **top-right of the tile thumbnail**, toggling `general.favorite` (and therefore the virtual
*Favorites* playlist). It does **not** change the selection (it stops propagation).

- The tile is a `motion.button` (`WallpaperGallery.jsx:659`) — a nested `<button>` is invalid HTML and Chromium
  auto-closes the outer button, which would restructure the tile's DOM and break its click/double-click
  handlers and the marquee hit test. The heart is therefore a **`<span role="button" tabIndex={-1}>`** overlay,
  not a button element.
- The heart renders **only for supported types**, so it never collides with the "Coming soon" badge that sits at
  `top:6; right:6` (`WallpaperGallery.jsx:696-712`).

The gallery toolbar (`WallpaperGallery.jsx:492`) gains a **Favorites** filter pill next to *Sort*. When it is on,
only favorited files are listed. Favorites always **sort first** (matching the engine's existing "Favorites First"
order at `engine.js:448`), then the user's chosen sort.

### 4.3 Drag & drop into the playlist

- `dragstart` on a tile writes `application/x-rex-wallpaper-paths` = JSON array of the **selected** paths (or just
  the dragged file when it is not part of the selection), `effectAllowed = 'copy'`.
- The dock strip is a drop target: `dragover` prevents the default, the strip highlights, `drop` appends the
  paths to the panel's current playlist (duplicates ignored).
- Dropping **on a chip** inserts before that chip instead of appending.

### 4.4 The dock header

```
[▤] Nature ▾        (config)                          -   []   X
```

- **Icon + name + caret**: the **playlist picker** — the list of playlists (icon + name), *New playlist…*, and a
  right-click context menu on any entry. The retired gallery cube's menu had **Settings · Rename · Icon ·
  Delete** (`WallpaperGallery.jsx:759-815`); *Duplicate · Export · Import* are added from the engine's existing
  `playlistEngine.manage` (`engine.js:985-1017`).
- **`(config)`**: a quiet **grey word** (not an icon) that opens the existing `PlaylistEngineDialog` scoped to the
  panel's current playlist (today it is opened from the cube's gear button, `WallpaperGallery.jsx:548`). The dialog is
  rendered through a **portal on `document.body`** — the dock clips its overflow, so a dialog mounted inside it would
  be cut off.
- **`-` `[]` `X`**: minimize / maximize / close the panel (§2.4).

The gallery toolbar's **playlist cube is retired** — its toolbar button becomes "open the dock". One playlist
manager, not two.

**The dock's playlist is sticky.** It remembers the playlist you picked (`ui.panelPlaylistId`) and does *not*
follow the focused wallpaper: a drag into "Nature" must not retarget the drop zone because the focus moved.
It is seeded once, on first use, from the focused wallpaper's active playlist.

### 4.5 The strip

Each chip is a **thumbnail-only cube** (130 px square, 14 px radius — the gallery's tile look): the poster fills it,
and the file's name lives in the tooltip / `aria-label`. No name text in the strip.

The strip is a **grid**, not a row: `repeat(auto-fill, 130px)` with a 10 px gap, so the cubes flow into rows as the
playlist grows and the strip scrolls vertically.

| Gesture | Result |
|---|---|
| Click a chip | select the file (the Inspector follows it) |
| Double click a chip | apply it to the desktop (mirrors the gallery) |
| Drag a chip | **move it** — reorder inside the playlist (the drop target lights up) |
| Hover a chip | a **remove** button fades in on its top-right corner |
| Right click a chip | *Apply · Open in Explorer · Set as active playlist · Remove from playlist* |
| Drop files on the strip | add them to the playlist |

The chip's remove button is a `<span role="button">`, **not** a `<button>`: a real button inside the cube's button
is invalid HTML and Chromium closes the outer button, which would break the cube's click/double-click handling. The
reorder drag uses its own mime type (`application/x-rex-playlist-reorder`) so it can never be confused with the
gallery's "add to playlist" payload, and the dragged index is kept in a ref so the drop handler knows the source even
before React re-renders.

An empty playlist shows a hint: *"Drag files here to add them to <playlist>"*.

### 4.6 The Inspector gains a thumbnail

A poster `<img>` block at the top of the Inspector, above the property sections, showing the focused file's
thumbnail (video files use their decoded poster frame). Missing poster → the type icon. With a multi-selection
the Inspector keeps showing the focused file, so nothing else about it changes shape.

---

## 5. Persistence

| What | Where | Shape | Default |
|---|---|---|---|
| Split width | `app-state.json` → `ui.gallerySplitPx` | number (px) | `null` → even 50/50 |
| Dock height | `ui.panelHeight` | number (px) | `220` |
| Dock open / minimized / maximized | `ui.panelOpen`, `ui.panelMinimized`, `ui.panelMaximized` | boolean | `true`, `false`, `false` |
| Dock's playlist | `ui.panelPlaylistId` | string \| null | `null` → the focused wallpaper's active playlist |
| Playlist definitions + members | `playlistEngine.playlists` | `[{ id, name, icon, members }]` | the built-ins (§3.1) |
| Favorite | **the gallery library** (`wallpaper-library.cjs`) | per-path `favorite` | `false` |

Rules:

- **Every new key must be in `defaultState()` *and* in `mergeWithDefaults`' explicit key list**
  (`app-state-manager.cjs:152-170`). `ui` merges key-by-key (`:159`), so the new `ui` keys ride along;
  `playlistEngine` does **not** — its merge is `{ ...(saved.playlistEngine || {}) }` (`:168`), which **drops the
  defaults**. That line must become `{ ...def.playlistEngine, ...(saved.playlistEngine || {}) }` or the default
  playlist list is lost on load.
- The library's `favorite` is written through `window.rexDesktop.gallery.update(path, { favorite })`
  (`preload.cjs:41`) — no dot-path setter, and no `wallpaperSettings` map is needed for it.
- **Sizes are written on drag end**, not per pointer-move (`useAppState` writes through to disk on every call).
- `useAppState` (`app/src/hooks/useAppState.jsx`) loads asynchronously and has no subscription — the dock's own
  state is seeded from it on mount, not mirrored live.

### 5.1 What survives a restart today, and what must now

| Thing | Today | After |
|---|---|---|
| Playlist names/icons (user-made) | **lost** — only the engine's manual backup/restore JSON keeps them | persisted in `playlistEngine.playlists` |
| Which files are in a playlist | **lost** — `playlist.currentPlaylist` lives on a record rebuilt from the file | persisted by path |
| Favorites | **lost** (the library has the field, nothing writes it) | persisted by path in the library |
| Panel/divider sizes | n/a | persisted |

---

## 6. Behaviour changes inside the existing code

| Place | Today | After |
|---|---|---|
| `engine.js:353-401` `filterPool` | pool = every store wallpaper | pool ∩ the active playlist's members (empty playlist ⇒ today's behaviour) |
| `store.js:91` `renamePlaylist` | rewrites `currentPlaylist` on every member | still rewrites the **active** playlist name on members whose active playlist is this one; **membership is by path and is untouched** |
| `store.js:105` `deletePlaylist` | resets each member's `currentPlaylist` to `'None'` | drops the playlist; a member whose *active* playlist was the deleted one falls back to `'None'`; other memberships survive |
| `registry.js:175` `DEFAULT_GENERAL.favorite` | `true` | `false`, hydrated from the library |
| `registry.js:386` `info.favorite` | present | left alone — **dead**: `InfoSection` is imported nowhere |
| `InfoSection.jsx:55` / `PlaylistSection.jsx` | toggle `info.favorite` / `currentPlaylist` | **not mounted** (only `AppearanceSection`, `RendererSection`, `ActionsSection` are — `Inspector.jsx:11-13`; `DisplaySection` is used by the gallery cube, `WallpaperGallery.jsx:24`). These sections stay dead; this milestone does not resurrect them. |
| `store.js:188` `loadWallpaperFile` | sets `selectedId` directly | must set `selectedPaths` too (§3.3) |
| `engine.js:1032` → `store.js:146` `restorePlaylists` | maps `{...p}` | keeps `members` (already a spread — verified) |
| `WallpaperGallery.jsx:516-645` | the playlist cube | deleted; the dock header takes over |
| `WallpaperGallery.jsx:108,601,623,848` | name-based `currentPlaylist` lookups | keep working — the field stays a name (§3.1) |
| `PlaylistEngineDialog.jsx:105,141` | name-based lookups | keep working — the field stays a name (§3.1) |

---

## 7. Files

| Path | Change |
|---|---|
| `app/src/App.jsx` | insert the split handle as a third child of `.wallpaper-grid`; make the page a flex chain (§2.2) |
| `app/src/components/SidebarLayout.jsx` | content column + dock slot; nav-width constants |
| `app/src/components/SidebarLayout.css` | `.content-col`, `.page-area`, `.dock`, split handle, marquee, strip styles |
| `app/src/wallpapers/PlaylistDock.jsx` | **new** — dock header, picker, context menu, drop target |
| `app/src/wallpapers/PlaylistStrip.jsx` | **new** — chips, drag-reorder, chip menu |
| `app/src/wallpapers/thumbCache.js` | **new** — path-keyed poster cache |
| `app/src/wallpapers/store.js` | selection set, playlist members, `ensureWallpaperForPath`, favorite mirror |
| `app/src/wallpapers/WallpaperGallery.jsx` | multi-select, marquee, heart, drag source, Favorites filter, cube retired |
| `app/src/wallpapers/registry.js` | favorite default |
| `app/src/wallpapers/inspector/Inspector.jsx` | thumbnail block |
| `app/src/wallpapers/playlist/engine.js` | pool = membership; delete/rename semantics; backup carries `members` |
| `app/electron/app-state-manager.cjs` | new keys; the `playlistEngine` merge fix |

**No change needed:** `wallpaper-library.cjs` and `preload.cjs` already provide the per-path `favorite` field, the
`update` IPC and the `list({favorite})` filter.

**Dead code (not in scope, do not revive):** `app/src/wallpapers/GalleryGrid.jsx` (windowed grid, imported
nowhere — the gallery renders every tile) and the unmounted inspector sections (`InfoSection`, `PlaylistSection`,
`GeneralSection`, `AudioSection`, `AdvancedSection`, `MonitorSection`, `InteractionSection`, `PlaybackSection`,
`ScheduleSection`, `PerformanceSection`).

---

## 8. Verification

**`pnpm dev` is browser-only** (`app/package.json:8` runs plain Vite): there is no `rexDesktop` bridge, so
`getPathForFile` does not exist and `record.path` is `null` for every file (`gallery.js:72`). Membership,
favorites and the drop payload are all path-keyed, so **the feature is inert in the browser dev server** — the
live passes below run in the **Electron app** (`pnpm --dir app electron`).

Automated:

- `pnpm lint` (oxlint) — 0 errors.
- `node --check` on every touched main-process file.
- `pnpm typecheck` is **not** a gate for this work: the root `tsconfig.json` includes only `src` (the retired
  Surface System), not `app/src`, and `app/tsconfig.json` has `allowJs` off, so it never sees the `.jsx` files
  this feature touches.

Live passes (Electron):

1. Drag the gallery/inspector divider → the inspector follows; the width survives a restart.
2. Drag the dock's top edge → the page area shrinks; the height survives a restart.
3. `-`, `[]`, `X` each do what §2.4 says; the page area reclaims the space.
4. Marquee over the gallery → the intersected tiles select; Ctrl+drag adds to the selection.
5. Ctrl+click toggles; Shift+click selects a range; a plain click replaces.
6. Drag a tile into the strip → it joins the playlist; drop it on a chip → it lands before that chip.
7. Click a chip → the Inspector shows that file; double click → it goes to the desktop.
8. The heart toggles; the Favorites filter lists only hearted files; favorites sort first.
9. With "Nature" holding 3 files, the playlist engine rotates **those** files, not the whole filtered pool.
10. Delete a playlist → it is gone, its files keep their other memberships, and the engine falls back cleanly.
11. Restart → sizes, favorites, playlist names, and membership are all still there.

---

## 9. Non-goals

Runtime playlist playback over IPC (still `TODO.md` P4) · a virtualized gallery grid · moving/renaming files on
disk · resurrecting the dead inspector sections · per-monitor layouts · anything in the native engine or the
runtime protocol.

---

## 10. Precautions

- **Do not** break the existing single-click-select / double-click-apply contract — the whole app relies on it.
- The heart and the rename input inside a tile must opt out of the tile's drag (`draggable={false}` +
  `stopPropagation`), and the heart must **not** be a `<button>` inside the tile's `<button>`.
- The marquee overlay must live **outside** the framer-motion-animated children and be positioned against
  `.gallery-grid` (the scroll container), not `.wallpaper-grid`.
- Write sizes on drag **end**; `useAppState` writes through to disk on every call.
- Membership is keyed by path; a Windows path contains dots, so it must never travel through a dot-path setter.
- The engine's pool change is one condition in one function — but the engine **applies wallpapers to the
  desktop**, so it needs a live check, not just a unit test.
- The dock is global, but the Wallpapers page is the only one with files — the dock must still render (and stay
  usable) on Settings/About.

---

## 11. Implementation status (2026-10-20)

### Shipped

| Piece | Where |
|---|---|
| Content column: page area on top, global dock at the bottom; the nav width is one constant set | `SidebarLayout.jsx`, `SidebarLayout.css` |
| Dock: resizable by its top edge (120 px … 70 % of the window), `-` minimize, `[]` maximize, `X` close, all persisted | `SidebarLayout.jsx`, `PlaylistDock.jsx`, `PlaylistDock.css` |
| Gallery ↔ inspector split: an 18 px drag handle between the columns, clamped to 280 px / 320 px, persisted | `App.jsx`, `ResizeHandle.jsx`, `SidebarLayout.css` |
| Dock header: playlist icon + name + picker (select / new / rename / icon / remove) + `config` (the engine dialog) + the three window controls | `PlaylistDock.jsx` |
| The strip: small poster + name per file, click = select, double click = apply, right click = apply / remove from playlist | `PlaylistStrip.jsx` |
| Playlist membership: path-keyed member lists, add / remove / reorder, persisted in `playlistEngine.playlists` | `store.js` |
| Drag a gallery tile into the strip → it joins the playlist | `WallpaperGallery.jsx` (`application/x-rex-wallpaper-paths`), `PlaylistStrip.jsx` |
| The gallery's playlist cube retired; playlist management lives in the dock header | `WallpaperGallery.jsx` |
| **The engine's pool = the active playlist's members** (empty playlist ⇒ the old filter-only behaviour) | `playlist/engine.js` (`filterPool`) |
| The page sizes to its box (the `calc(100vh - 252px)` height is gone) and the split is the only viewport constant left | `SidebarLayout.css` |
| **The app's theme is now `dark`** (`<html class="dark">`) — the app rendered the *light* shadcn theme on entirely dark surfaces, so every shadcn component was near-black on black: the Inspector's text, the playlist settings dialog, the dropdown menus. Found live while checking the pop-up's contrast. | `app/index.html` |
| **The default split is the measured one** — gallery 77.1 % / inspector 22.9 % (measured: gallery 1723 px, inspector 511 px), stored as a *ratio*, not pixels, so a fresh install starts there and the layout holds at any window size. A saved pixel split still wins for an existing install. | `App.jsx`, `SidebarLayout.css` |
| The **minimized strip drops the resize handle** so the 36 px header owns the whole strip — the handle was pushing the header out of the panel and cropping it. The button trio is always `- [] X`. | `PlaylistDock.jsx`, `PlaylistDock.css` |

### Held (still to do — the spec above is the contract)

- **Multi-select** (rubber-band marquee, Ctrl/click, Shift/click) — the store still has a single `selectedId`.
- **The heart + the Favorites filter** — and with them the *favorite* cleanup: `general.favorite` still defaults to
  `true` and `info.favorite` still exists (its only writer, `InfoSection`, is unmounted). Until the heart lands,
  **"Favorites" behaves as an ordinary container** rather than a virtual one derived from the flag.
- **The Inspector thumbnail** — the shared poster cache (`thumbCache.js`) exists and is populated, but the Inspector
  does not render a poster yet.
- Dropping gallery files **onto a specific cube** (insert before it) — today they are appended to the playlist.
- The `wallpaperSettings[path]` map (favorites + active playlist) — the durable home for favorites is still the
  gallery library's unused per-path `favorite` field.

### Deviations from the spec

- The dock's playlist is **sticky** (§4.4) and defaults to the first playlist that holds files, then to the first
  playlist — not to the focused wallpaper's active playlist.
- The dock's resize handle is 6 px; the split handle is 18 px (it replaces the panels' old 18 px gutter).
- The dock's edges are **rounded like the gallery panels** (18 px, all four corners — it floats) — user request during
  the live pass.
- The dock **floats 14 px above the window's bottom edge** (the content column's bottom padding) — user request.
- The gap between the panels and the dock is **10 px**: the page area's bottom padding went 24 px → 10 px and
  `.content-section`'s 28 px bottom margin is gone (the old stacked-page spacing; the dock provides the separation).
- **The dock's width equals the gallery + inspector width** (user request): the 24 px gutter moved from the page area
  to the content column, so the dock's left/right edges line up with the panels' instead of running to the window's
  edges.
- The persisted split is **clamped at render time** to the width that actually fits (`gridWidth - 320 - 18`), so a
  saved split wider than the window can never push the panels off-screen. The saved value is kept, so the layout
  returns to it when the window grows again.
- The `config` entry is a **grey word** rather than an icon, and the playlist settings dialog is **ported to
  `document.body`** so the dock's `overflow: hidden` cannot clip it — both user requests during the live pass.
- The dock has **one inset** (`--dock-pad: 10px`) shared by the header and the strip, so the playlist picker, the
  `config` word, the window controls and the strip's cubes all line up on the same edges; the cubes sit 8 px under
  the header. The gap between the dock and the panels above it is 10 px, and the dock floats 14 px above the window's
  bottom edge.
- The **layout state store keeps a change made while the persisted state is still loading** (`layout.js`): the async
  load used to overwrite the in-memory `ui` wholesale, so a click landing in that window (e.g. *minimize*) was
  silently undone — and then persisted as undone. The touched keys are now preserved across the load.
- The app shell's window controls were **re-ordered to `- [] X`** (yellow minimize → green maximize → red close) to
  match the dock's panel controls; their colors and functions are unchanged.
