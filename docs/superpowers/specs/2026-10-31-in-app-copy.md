# In-app copy — man-page minimal (2026-10-31)

## Intent

Rewrite the in-app descriptions before the Store release so they read like
notes from the engine's author: terse, technical, one hook per line. The
current copy is clear but generic ("Start Paiint when you sign in to
Windows"); it could belong to any tray app.

## Audience

Developers and technical users choosing a wallpaper engine. They know what a
frame, a decode, a scene and a tray are. The copy should reward that
knowledge without becoming cryptic.

## Voice

**C — man-page minimal.** Fragments over sentences. Precise nouns. "=" for
state mappings. One concrete consequence or hook per description. No
marketing adjectives, no exclamation marks.

## Scope

- `app/src/App.jsx` — the three page subtitles.
- `app/src/settings/SettingsPage.jsx` — every static row description and the
  engine-offline hint.
- `app/src/about/AboutPage.jsx` — tagline, blurb, privacy note, FFmpeg note.

Not in scope: labels, buttons, dynamic status strings, empty states, the
Store listing, `package.json` / `AppxManifest.xml` descriptions.

## The deck

### Page subtitles

| Where | Now | New |
|---|---|---|
| Wallpapers | Your library, your playlists, one click to the desktop. | Library. Playlists. Desktop. |
| Settings | How Paiint behaves on your machine. | Runtime configuration. |
| About | Paiint — the wallpaper engine. | The build, the engine, the licences. |

### Settings rows

| Row | Now | New |
|---|---|---|
| Launch at login | Start Paiint when you sign in to Windows | Start with your Windows session. |
| Start minimised | Start in the tray without opening the window | Boot to tray. No window. |
| Restore the last wallpaper at launch | Put the wallpaper back on the desktop when Paiint starts | Re-apply the last scene. |
| Remember window size and position | Restore where you left the window on the next launch | Persist size and position. |
| Quit Paiint when the window closes | Stops the wallpaper and leaves the last frame on the desktop. Turn this off to keep Paiint running in the tray. | Stop engine. Keep last frame. Off = stay in tray. |
| Frame rate cap | Off follows the monitor. Applies when the wallpaper engine starts | Off = display refresh. Applied at engine start. |
| Hardware video decoding | Off uses the CPU. Applies when the wallpaper engine starts | GPU default. Off = CPU decode. Applied at engine start. |
| Leave the last frame on exit | When Paiint quits, save the final frame of a playing video as a still wallpaper so the desktop keeps a picture | Freeze the final frame as a still. No black desktop. |
| Play audio only when the desktop is clear | Mutes the wallpaper when the desktop is covered by another window (or a Paiint window is open). Audio returns when the desktop is clear. | Mute when covered. Unmute when clear. |
| Test mode diagnostics | Record engine, display and device details to Downloads\Paiint Diagnostics while Paiint runs. Local only — nothing is uploaded. | Local-only engine log. Downloads\Paiint Diagnostics. No upload. |
| Default fit | How a newly imported wallpaper fills the screen | Fit mode for new imports. |
| Clear posters | Cached preview images are rebuilt on demand | Preview cache. Rebuilt on demand. |
| Engine offline hint | The wallpaper engine is not running. Apply a video wallpaper to see it live. | Engine offline. Apply a video wallpaper to start it. |

### About page

| Field | Now | New |
|---|---|---|
| Tagline | Live wallpaper engine for Windows | Native wallpaper engine for Windows |
| Blurb | Video, HDR and still wallpapers rendered on the desktop itself, behind your icons. When you quit, the last frame stays on the desktop as a still. | Video, HDR and still wallpapers, rendered behind your icons. Native C++/D3D12 runtime. Quit, and the last frame stays. |
| Privacy note | Paiint collects nothing and makes no network requests. Wallpapers, playlists and settings stay on this PC. | No telemetry. No network. Wallpapers, playlists and settings stay on this PC. |
| FFmpeg note | Bundles FFmpeg (LGPL-2.1) and other open-source components. Every licence text — including all bundled JavaScript packages — ships with the app. | Bundles FFmpeg (LGPL-2.1) and other open-source components. Every licence text ships with the app. |

## Deliberate omissions

- **HDR stays in the blurb.** The app-hosted HDR swapchain fix is in progress
  and will ship in an update; the Store/manifest copy keeps promising it too.
- "Live" is dropped from the tagline; "native" is the differentiator that
  matters to this audience.

## Acceptance

- Every string above appears in the three files; no other UI string changes.
- `pnpm lint`, `pnpm typecheck`, `pnpm build` stay green.
- The copy fits the existing two-line description slot at 10.5 px; no layout
  changes.
