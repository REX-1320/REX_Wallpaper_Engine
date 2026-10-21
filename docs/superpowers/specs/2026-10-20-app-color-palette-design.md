# REX App Colour Palette — Research & Design

**Status:** implemented 2026-10-20 · **Scope:** the Electron app's own chrome (`app/src/**.css`).
**Related:** [`2026-10-20-gallery-layout-playlist-design.md`](2026-10-20-gallery-layout-playlist-design.md).

---

## 1. The research

Five sources, all from the platform REX actually lives on:

| Source | What it gave |
|---|---|
| [Fluent 2 — Color](https://fluent2.microsoft.design/color) | Three palettes (neutral / shared / brand); semantic colours for status, used sparingly and never for decoration; **one accent** for interactivity and state; "don't use colour as the only differentiator" |
| [Color in Windows](https://learn.microsoft.com/en-us/windows/apps/design/signature-experiences/color) | In dark mode **darker = less important**, important surfaces are lighter/brighter; accent values are generated for contrast and used sparingly; colour-blind safety (≈8 % of men are red-green deficient) |
| [Layering and elevation](https://learn.microsoft.com/en-us/windows/apps/design/signature-experiences/layering) | **Two layers** — a *base* layer (menus, commands, navigation) and a *content* layer (the central experience); every elevation level carries a **1 px contour**; shadows scale with elevation (Layer 1 · Control 2 · Card 8 · Flyout 32 · Tooltip 16 · Dialog 128) |
| [Acrylic material](https://learn.microsoft.com/en-us/windows/apps/design/style/acrylic) | Acrylic is for **transient, light-dismiss surfaces** (menus, flyouts); "don't put desktop acrylic on large background surfaces"; "don't place accent-coloured text over acrylic"; it degrades to a **solid** colour under High Contrast, transparency-off, Battery Saver or on low-end hardware |
| [Accessible text requirements](https://learn.microsoft.com/en-us/windows/apps/design/accessibility/accessible-text-requirements) | **4.5:1** minimum for visible text (WCAG 2.2 G18); inactive UI is exempt; use theme brushes rather than hard-coded colours |
| [Fluent dark-mode token values](https://learn.microsoft.com/en-us/visualstudio/extensibility/ux-guidelines/theme-color-token-reference) (Windows 11 / Fluent, dark column) | The actual ramp: `ControlFillDefault` 5 % · `ControlFillSecondary` 8 % · `ControlFillTertiary` 3 % · `ControlAltFillSecondary` 10 % · `ControlStrokeDefault` 7 % · `LayerFillAlt` 5.5 % · `ControlSolidFillDefault` #454545 |

## 2. The audit that motivated it

| measure | before |
|---|---|
| distinct colour values in the app's own CSS | **136** (447 uses, 8 files) |
| alpha steps of "white" used for text | **22** (9 of them in active use) |
| text steps failing 4.5:1 | `rgba(255,255,255,0.45)` → **4.41:1** (8 uses) |
| contrast between the nav and the dock surfaces | **1.04:1** — indistinguishable by fill |
| spellings of the accent | `#7dd3fc` · `#38bdf8` · `rgba(56,189,248,…)` (+ `#a78bfa` as a second accent) |
| danger reds | `#ef4444` (5.58:1) and `#dc2626` (**4.35:1 — fails AA**) |
| shadcn token set (dark, now active) used by the app's own CSS | **0 rules** — two parallel colour systems that never met |

## 3. The palette — "Obsidian & Champagne"

One dark palette, in `app/src/index.css` (the `:root` block, so it is available to Tailwind and to plain CSS alike).

**Direction:** the neutrals are warm obsidian (never blue-black) and the whites are champagne-tinted, so the whole
surface reads as *one material* rather than as grey plus a gold accent bolted on. Chosen from four researched
directions (Graphite+Amber, Midnight Indigo+Mint, Fluent+system accent, Slate+Violet).

### Surfaces — Windows' two-layer model

```
--surface-base      rgba(13, 12, 11, 0.88)   nav + dock          (the BASE layer: chrome)
--surface-content   rgba(24, 22, 19, 0.92)    gallery + inspector (the CONTENT layer: the work)
--surface-raised    rgba(36, 32, 27, 0.97)    menus, flyouts, the dialog
--surface-sunken    rgba(0, 0, 0, 0.50)        wells, scrims
--surface-smoke     rgba(0, 0, 0, 0.55)        the modal scrim (Fluent's "smoke")
```

The content layer is **lighter** than the base layer, because in dark mode a surface of greater importance is lighter.

### Text — champagne-tinted white, four steps

```
--text-primary     rgb(255, 248, 235)          18.7:1   titles, active items
--text-secondary   rgba(255, 248, 235, 0.85)   13.4:1   labels, body
--text-tertiary    rgba(255, 248, 235, 0.62)    7.4:1   hints, metadata
--text-disabled    rgba(255, 248, 235, 0.38)    3.4:1   inactive UI only — always paired with an icon
--text-inverse     #17130e                      for text on a light chip
```

### The champagne ramp — one hue, five shades, each assigned to where it is used

```
--accent-bright #f3e3c3   15.6:1 on the base   accent text on the darkest layer
--accent        #e6cfa2   13.0:1               the default accent text and icons
--accent-core   #d9b779    9.4:1 as a fill      accent chips, selected states (dark text on it)
--accent-deep   #b9975c    6.0:1               accent text on the lightest (raised) surface
--accent-shadow #8f7343    4.4:1               decorative only — never text
```

Plus the tints: `--accent-muted` (14 %), `--accent-subtle` (8 %), `--accent-border` (55 %), `--accent-border-strong`
(90 %), `--accent-text` (the dark text on an accent fill).

### Contours, fills, status

```
--stroke-subtle .08   --stroke-default .16   --stroke-strong .28    (champagne hairlines, not white)
--fill-rest .05   --fill-hover .08   --fill-pressed .03   --fill-strong .10   --fill-selected .16
--danger #f2856f (7.9:1)   --warning #e8b23c (10.2:1)   --success #8fbf8f (9.4:1)
```

The hairlines are **champagne, not white** — a warm contour is what makes a dark surface read as metal rather than as
flat grey. They sit at 1.12 / 1.32 / 1.79:1 against the base surface, which is what the panels' edges need.

The traffic-light buttons keep the OS colours (`--win-close/-minimize/-maximize`) — they mirror platform controls and
deliberately are not part of the palette. The per-wallpaper-type gradients stay too: they are categorical, not semantic.

## 4. What changed in the app

1. **80 tokens** in `index.css` — the single source of truth.
2. **257 declarations** across `SidebarLayout.css`, `PlaylistDock.css`, `media-controls.css`,
   `inspector/wallpaper.css` and `Glass_effect.css` migrated to them, by a **role-aware** mapping (the same literal is
   a text colour in one property and a fill in another, so a blind find/replace would be wrong). The migration script
   is kept at `app/tools/migrate-palette.cjs` — it is idempotent and reports anything it cannot map.
3. **The content layer became an occluding surface.** The gallery and the inspector panels were transparent (or near
   black) while the chrome text on them was near-black — so the gallery's title was invisible on a dark panel and the
   page title was invisible on a light wallpaper. They now carry `--surface-content`, and the page title (which sits
   on the window) carries its own shadow.
4. **The title bar's controls got a surface.** The search pill and the profile button float over the window, so they
   now use `--surface-base` with light text instead of a 6 %-black smudge with dark text.
5. **The failing text step was raised** from 0.45 (4.41:1) to 0.62 (7.4:1), and the second danger red
   (`#dc2626`, 4.35:1) was replaced by `#f2856f` (7.9:1).
6. **The champagne appears in the chrome**, not only in the palette file: the active nav item is a champagne chip
   (`--accent-core` with dark text), the selected gallery tile gets a champagne ring, the nav hover is a champagne
   tint, and the media controls' accent gradients were re-tinted from sky→violet to champagne.
7. **The playlist dock is scoped to the Wallpapers page** — the playlists are wallpapers' playlists, so the dock does
   not follow the user onto Settings/About. Its size and state are remembered either way.

## 4.1 The background effect

The window is **not** transparent: it uses `backgroundMaterial: 'mica'` (`main.cjs:296`), so **DWM draws the blurred,
tinted desktop wallpaper behind the app for free** — the same material Windows Terminal uses. That changed what the
background layer had to be:

- It used to be an **opaque white fill**, which hid the Mica completely — the app had no backdrop at all.
- It is now a **translucent champagne wash** over the Mica: a corner glow (`rgba(217,183,121,0.10)`), a cooler one
  opposite, a top sheen, and a warm obsidian tint at 42 %. The Mica shows through the rest.
- The **panels** carry the material: the nav and the dock get the corner glow and a lit top edge
  (`inset 0 1px 0 rgba(217,183,121,0.22)`), the content panels get a soft sheen from the top.
- **No filters anywhere.** A `backdrop-filter` on the dock (a full-width strip) was removed: a live blur is a
  per-frame GPU pass competing with the wallpaper's own D3D12 present — the exact lesson from 2026-10-11, when a
  full-window blur was the #1 GPU consumer. The effect is painted with gradients, which are rasterised once.
  Verified live: the dock's `backdrop-filter` and the glass layer's `filter` both compute to `none`.

## 5. The guard (why it cannot drift back)

`app/tools/check-palette.cjs` (`pnpm --dir app check:palette`) fails the build when:

- **any raw colour literal** appears in the app's own CSS (shadows, gradients and `transparent` are exempt — they are
  depth cues, not surfaces);
- **any `var()` reference** does not resolve to a defined token (this caught the media controls' `--accent-strong`
  and the Mica backdrop's `--mica-blob` / `--mica-tint`, which were **never defined**);
- **any palette text step** falls below 4.5:1 against a surface it is used on (the disabled step is exempt as inactive
  UI, and the accent/status steps are checked against the base surface).

## 6. Verified in the live app

Measured against the *rendered* backgrounds with the Chrome DevTools Protocol:

| element | ratio | AA |
|---|---|---|
| nav label (the champagne chip) | 18.5 : 1 | ✓ |
| page title | 19.9 : 1 | ✓ |
| gallery title (was 1.17 : 1) | 17.1 : 1 | ✓ |
| "New" pill | 17.1 : 1 | ✓ |
| inspector title | 17.5 : 1 | ✓ |
| dock picker / `config` | 18.5 : 1 | ✓ |
| title-bar icon | 18.5 : 1 | ✓ |
| playlist settings dialog (title / section / body / button) | 16.5 / 16.5 / 16.0 / 16.0 : 1 | ✓ |
| playlist picker menu item | 12.0 : 1 | ✓ |

And structurally: the Mica shows through the window (the champagne wash over DWM's blurred wallpaper), the active nav
item is the champagne chip, the dock's `backdrop-filter` and the glass layer's `filter` both compute to `none`, and the
dock disappears on Settings and returns with its remembered size on Wallpapers.

`pnpm lint` 0 errors · `pnpm --dir app build` clean · `pnpm --dir app check:palette` clean.

## 7. Non-goals

Light mode (the app is dark by design; the tokens live in `:root` so a light block can be added later) · the wallpaper
type gradients · the OS traffic-light colours · tinting the palette with the user's accent colour (Windows does this
per-user; REX has a fixed brand hue today).
