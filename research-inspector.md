# REX — Inspector usage research (2026-10-21)

**Question:** which controls belong in the wallpaper Inspector, ordered by what
people actually use in wallpaper apps — and which of those can REX apply today?

**Verdict labels:** LIVE = reaches the engine today · FOUNDATION = code exists,
not wired · GAP = missing.

Method: full code audit of what the runtime accepts and what the UI applies,
inventory of every inspector section (mounted or not), teardown of the two
reference products (Wallpaper Engine, Lively Wallpaper) plus Windows
personalization, and their support-doc themes. No public per-control analytics
exist for any of these apps; §7 proposes a local measurement plan.

---

## 1. The hard constraint: six live properties

The Inspector can only promise what the runtime can change without a reload.
Today that is six fields on the **applied** wallpaper
(`DesktopApply.jsx:37-46` → `wallpaper:update`, `main.cjs:620`):

| Property | Engine command | UI today | Persisted per wallpaper |
|---|---|---|---|
| `display.mode` (fill/fit/stretch/center/tile) | `SET_FILL_MODE` / image `LOAD` | Display dialog only (`WallpaperGallery.jsx:475`) | no |
| `playback.loop` | `SET_LOOP` | **none** | no |
| `playback.mute` | `SET_MUTE` | **none** | no |
| `playback.volume` | `SET_VOLUME` | **none** | no |
| `playback.speed` | `SET_SPEED` | **none** | no |
| `playback.audioMode` | `SET_AUDIO_MODE` | **none** | no |

Three more engine capabilities exist but are not forwarded by the app:

- `SET_BRIGHTNESS`, `SET_CONTRAST`, `SET_SATURATION` — accepted by the runtime
  (`rex-core/src/wallpaper_cli_main.cpp:1116-1118`), absent from the
  `wallpaper:update` IPC.
- `PAUSE` / `PLAY` — implemented for the tray (`main.cjs:473-499`,
  `tray-manager.cjs:79-82`); no renderer IPC, so the app window cannot pause.
- `GET_CAPABILITIES` — returns the renderer's real property list
  (`wallpaper_cli_main.cpp:1421`); the inspector never asks for it.

Two plumbing facts that limit everything above:

1. **Live updates need an applied wallpaper.** `DesktopApply` sends `SET_*`
   only when `active = appliedId` matches and the loaded scene is current;
   single-click selection is preview-only by design (`store.js` `applyWallpaper`
   vs `selectWallpaper`). The inspector edits the *selected* wallpaper, so a
   change to a non-applied tile is a draft that does nothing on screen.
2. **Per-tile settings do not persist.** `updateWallpaperProperty`
   (`store.js:73`) mutates the in-memory store only. Gallery entries are rebuilt
   from the library at launch (`gallery.js buildFileRecord`), so volume/speed/
   loop/fit revert to defaults. Only `last-wallpaper.json` keeps the *applied*
   scene's parameters for startup re-apply.

## 2. What the Inspector shows today

Mounted sections (`Inspector.jsx:113-115`): **Appearance, Renderer, Actions**.
Ten other section files exist and are imported nowhere (Display is the
exception — it is reused by the gallery dialog). So the panel is a small
subset of a much larger, dormant spec surface.

| Section | Controls | Reality |
|---|---|---|
| Appearance | 17 property controls (opacity, brightness, contrast, gamma, saturation, hue, temperature, vibrance, exposure, sharpen, blur, vignette, noise, grayscale, invert, tint, overlay, color) | **0 consumers.** No code outside `AppearanceSection.jsx` reads `wallpaper.appearance.*`. Three of them (brightness/contrast/saturation) have engine backing but are not plumbed. |
| Renderer · Video | codec dropdown, hardware decode, frame skip, loop, start position, subtitle file | **0 consumers.** Codec is a property of the file, HW decode is process-wide, the runtime has no subtitle track, `renderer.loop`/`startPosition` duplicate playback fields. |
| Renderer · Image | zoom, pan, Ken Burns, slideshow, transition | 0 consumers. |
| Renderer · Web/Scene/Shader/Audio-visualizer | URL, browser engine, camera, lighting, shader path, FFT… | Deferred types the app refuses to apply at all (`desktop.js:73-75`). |
| Actions | Duplicate, Open folder, Delete | **Real** (duplicate is store-only). |

Net result: **0 of the 6 live controls are exposed in the Inspector; ~30 inert controls are.**
A user opening the panel to fix "it's cropped" or "audio is playing" finds
nothing that works — while the search field diligently matches dead properties.

## 3. What the reference products put in front of users

- **Windows Personalization** exposes exactly one wallpaper setting: fit / fill
  / span / tile / center. Scaling is the universal wallpaper control.
- **Wallpaper Engine** — selecting a wallpaper shows a large preview, identity,
  and an author-defined **Properties** panel (sliders/colors/combos defined by
  the wallpaper), plus playback/audio; quality, frame rate and pause rules live
  in app Settings. Its help index is dominated by video playback, audio,
  performance and multi-monitor pages — the themes users actually hit.
- **Lively Wallpaper** — per-wallpaper "customize" plus app-level video player,
  scaling default, GPU decode toggle, and a first-class **pause algorithm**
  (fullscreen / battery / per-app) documented as a core performance feature.

The pattern is consistent: **preview → apply → scaling → audio → pause**, with
exotic per-wallpaper knobs supplied by the content author and performance
controls kept global.

## 4. What people use most (ranked, with confidence)

1. **Apply / switch wallpaper** — certain. It is the primary action of every
   wallpaper app. REX: tile double-click / Apply in the gallery; the inspector
   has no apply action at all.
2. **Fit / fill / scaling** — very high. The only toggle Windows itself offers,
   and the first thing every user looks for when a wallpaper is cropped.
3. **Pause wallpaper** — very high. Top support theme ("pause while gaming /
   working / on battery") in both reference apps; REX already implements it but
   hides it in the tray icon.
4. **Mute / volume** — high. The chronic audio complaint; every video-wallpaper
   app exposes it. REX has the engine commands but no UI.
5. **Playlist add / organize** — medium-high. The dock exists; the inspector
   cannot add the selected wallpaper to a playlist.
6. **Reveal / delete / cleanup** — medium. Used at cleanup time, not daily.
7. **Loop / speed** — medium-low. Loop is set-and-forget; speed is a niche
   effect. Both are one-line controls, so they earn a place once they are live.
8. **Brightness / contrast / saturation** — low-medium. Engine supports all
   three; no reference product offers generic color grading, and users normally
   fix brightness at the monitor/OS level. Worth a collapsed group only if
   measurement shows demand.
9. **Per-wallpaper monitor targeting** — low today. The runtime is
   single-desktop; a picker would be theatre.
10. **Author-defined properties (WPE-style)** — not applicable: REX content is
    plain video/image files, nothing declares properties.

## 5. Recommendation — Inspector v1

Order top → bottom:

1. **Poster** — keep the 450 px block, but cap it responsively
   (`min(450px, 55vh)`) so properties stay above the fold on short windows.
2. **Identity** — name, `type · resolution · duration · size`, applied badge.
3. **Primary actions** — **Apply** (when not applied) / **Applied**, and
   **Pause · Resume** when applied. These are the two highest-use actions and
   both already exist underneath.
4. **Playback** (video only) — Mute + Volume, Speed 0.25–2×, Loop, Audio output
   mode. Live via `SET_*`; disabled with a one-line hint when the wallpaper is
   not applied.
5. **Display** — fit mode as a segmented control (Fill / Fit / Stretch / Center
   / Tile). One source of truth shared with the gallery's Display dialog.
6. **File** — path (truncate from the left), Copy path, Open location.
7. **Playlist** (collapsed) — add the wallpaper to a playlist. Duplicate/Delete
   were dropped from the panel: they only mutated the store (the tile and the
   library entry survived) and the gallery tile menu already owns them.
8. **Delete** the Appearance section. Trim RendererSection to a compact
   read-only media block (codec/resolution/fps/duration) — no editable fields.

Rules:

- Render a control only if `effectiveCapabilities()` says the runtime supports
  it for that type; never show a control that cannot work.
- Keep the applied-vs-selected distinction explicit: an unapplied selection
  shows "Not on desktop · Apply" instead of silently discarding edits.
- Persist per-wallpaper settings (fit/volume/mute/loop/speed/audioMode) in the
  library entry for that path, so they survive relaunch; re-apply them on load.

**Do not build yet**

- WPE-style author-defined dynamic properties (no content format uses them).
- Generic color grading beyond the three engine-backed knobs.
- Per-wallpaper performance/auto-pause (global concern; P3-4 held).
- Monitor picker (engine has no per-monitor target).

## 6. Why this shape

The inspector's job is "what is this, do I want it on my desktop, and how should
it behave" — not a color-grading suite. In practice the decision is: apply it,
make it fit, decide whether it makes noise, pause it while working. Everything
else is occasional. Removing the inert controls is not a loss: dead controls
are the reason the working ones cannot be trusted today.

## 7. Measurement plan (make "most used" data, not opinion)

- Add local-only counters to the existing `app-state.json`
  (`usage.inspector.<action>`), no network, with a reset in diagnostics.
- Count: apply, pause, resume, fit change, mute, volume commit (on release),
  speed, loop, reveal, delete, playlist add, and inspector opens per session.
- After ~2 weeks of real use, prune v1.1 against the table. Until then the
  ranking in §4 is competitor- and support-theme-derived, not measured.

## 8. Open decisions

1. Move fit mode out of the gallery's Display dialog into the inspector (one
   home), or mirror it in both?
2. Apply on first edit, or keep draft edits + an explicit Apply button?
3. Is Pause global chrome (always visible, e.g. gallery toolbar) or
   inspector-only?
4. Expose brightness/contrast/saturation as a collapsed "Picture" group now, or
   wait for measured demand?

## Evidence index

- Live property set: `app/src/wallpapers/DesktopApply.jsx:37-46`,
  `app/src/wallpapers/desktop.js:81-112`, `app/electron/main.cjs:620`.
- Engine extras: `rex-core/src/wallpaper_cli_main.cpp:1116-1146`
  (SET_*), `:1421` (GET_CAPABILITIES), `main.cjs:473` (pause/resume).
- Inspector mounts: `app/src/wallpapers/inspector/Inspector.jsx:113-115`;
  Display reuse `app/src/wallpapers/WallpaperGallery.jsx:23,475`.
- Dead fields: `AppearanceSection.jsx` (20 controls), `RendererSection.jsx:47-69`;
  no consumers outside the inspector for `appearance.*` / `renderer.*`.
- Non-persistence: `store.js:73` (`updateWallpaperProperty`), `gallery.js:45`
  (`buildFileRecord`).
- References: Windows Personalization (fit/fill/span/tile/center); Wallpaper
  Engine help index (`help.wallpaperengine.io`) — playback/audio/performance-
  dominated; Lively wiki `Video-Guide.md`, `Performance.md` (pause algorithms,
  GPU decode, app-level settings).

## 9. What shipped (2026-10-21)

The recommendation above was built as **P3-7** (see `TODO.md` /
`everything.md`). Deltas from the plan, and what the build taught us:

- **Playback + Display became the whole panel.** The read-only media block was
  dropped: the app has no real metadata (the registry's resolution/size/
  duration are placeholders), so a "media facts" section would have repeated
  fake numbers. The panel shows the file's real size instead, read from disk.
- **Duplicate and Delete left the inspector.** They only mutated the store, so
  the tile and the library entry survived — a no-op behind a danger button.
  The gallery tile menu deletes for real.
- **Tile is not offered for videos.** The engine answers
  `SET_FILL_MODE failed: Tile fill mode is not implemented`, which surfaced as
  a user-facing error toast. Tile is an OS-only mode (Windows tiles images
  natively), so it survives for images only.
- **Audio controls now follow the file.** A container probe reads the header
  (MP4 `hdlr`, Matroska `TrackType`); silent clips show "This file has no
  audio track." and lose mute/volume/output. This had to be a container-level
  parse: Chromium cannot decode the AC-3 / DTS-HD MA audio in this library, so
  a decoder-based probe would have called every movie silent. The engine knows
  the truth too, but it only holds the applied scene and its binary is
  mid-flight, so it was not rebuilt for this.
- **The "on desktop" state had to be adopted from main.** Main re-applies
  `last-wallpaper.json` at startup, so the renderer's store believed nothing
  was applied and offered "Apply" for the wallpaper that was already on the
  desktop — and never sent live `SET_*` for it. The renderer now adopts that
  scene (`wallpaper:last-applied`) without re-loading it.
- **The panel could not scroll at all.** The 450 px poster consumed the whole
  column, leaving the scroll area 0 px tall, so the properties were clipped
  and unreachable. The poster now shrinks and the scroll area keeps a floor.
- **Still open:** per-wallpaper performance/auto-pause (global concern, P3-4
  held), brightness/contrast/saturation (engine-backed but unmeasured demand),
  monitor targeting (the engine is single-desktop), and a WPE-style author
  property system (no REX content format declares properties).
