# Paiint — Microsoft Store listing (paste-ready, version 0.1.0)

> Voice: man-page minimal. HDR stays. Support address: `support.paiint@gmail.com`.
> Counts verified 2026-11-03. Field limits: short description 256 · description ~1,900 target (< 3,000 safe) · features 200 each · keywords 7.

## Short description (182 / 256)

```text
Native wallpaper engine for Windows. Video, HDR and stills. Playlists. Multi-monitor. Pauses playback when your windows cover the desktop. Local files only. No account. No telemetry.
```

## Description

```text
Paiint renders your desktop.

A native wallpaper engine for Windows. Video, HDR video and still wallpapers play behind your icons through the desktop layer Windows itself uses - not a browser window stretched over the screen.

Local. Wallpaper files and playlists stay in your user folder; a diagnostics copy lands in Downloads when you ask for it. No account. No analytics. No network. No crash upload.

VIDEO
Import any local file: mp4, webm, mkv, mov and friends. Media Foundation and ffmpeg decoders. Fill, fit, stretch or centre. Speed. Loop. Seek. Brightness, contrast, saturation.
Sound when you want it: wallpaper audio plays until your windows cover the desktop, then mutes - and unmutes the moment the desktop clears.

HDR
An HDR video plays at HDR on an HDR display. SDR video and stills stay clean beside it - no washed-out red channel, no double tone map.

PLAYLISTS
Queue wallpapers. Sequential or shuffled order, repeat playlist or repeat current, a change timer in hours and minutes, transitions with a live preview. One click applies and arms the schedule.

MULTI-MONITOR
Every display gets its own wallpaper: independent assignment per monitor, with adapter and display changes detected and re-handled.

ENGINE
D3D12. The gallery window is software-composited - zero VRAM spent on the UI - and decoded video frames share a pool, so switching wallpapers keeps VRAM flat instead of stacking copies.

DESKTOP
Instant quit: the last frame is saved to Pictures\Paiint as a still and stays on the desktop after exit. Closing the window quits by default (keep-in-tray is a setting). Launch at login. Runs from tray.

SUPPORT AND LEGAL
Support: support.paiint@gmail.com
Privacy policy: https://rex-1320.github.io/Paiint/docs/legal/PRIVACY.html
License terms: https://rex-1320.github.io/Paiint/docs/legal/EULA.html

Paiint. By MoiNeX. Less browser, more desktop.
```

## What's new (301)

```text
0.1.0 - first Store release.

Video, HDR and still wallpapers on a native desktop layer. Playlists with timers and transitions. Independent per-monitor assignment. Occlusion-aware audio. Instant quit with a last-frame snapshot. Launch at login. Local files only - no account, no analytics, no network.
```

## Features (10, max 120 chars each)

1. `Native desktop rendering: video, stills, HDR, web and shader wallpapers on the real desktop layer.`
2. `HDR video plays correctly on HDR displays; the SDR pool stays clean, no washed-out red channel.`
3. `Occlusion-aware audio: wallpaper sound mutes when your windows cover the desktop and unmutes when it clears.`
4. `Playlists with change timers, sequential or shuffled order, repeat modes and transitions with preview.`
5. `Independent wallpaper per monitor, with adapter and display-change detection.`
6. `Full playback controls: speed, loop, seek, fill/fit/stretch/centre, brightness, contrast, saturation, volume.`
7. `Instant quit: the last frame is saved into Pictures\Paiint and stays as a still wallpaper.`
8. `Launch at login; start minimised to tray; keep-in-tray optional.`
9. `Software-composited gallery UI: zero VRAM spent on the settings window; a decoded-frame pool keeps video VRAM flat.`
10. `Local-only: import any local file, no account, no analytics, no network, no crash upload.`

Features are numbered list items, each ≤ 200 chars. Measured chars: 98, 95, 108, 102, 77, 109, 90, 64, 115, 89.

## Keywords (7)

```text
wallpaper, video wallpaper, HDR, playlist, multi-monitor, live wallpaper, desktop
```

## Copyright line

```text
Copyright (c) 2026 MoiNeX
```

## Other fields

| Field | Value |
|---|---|
| Product name | Paiint (fixed by package identity) |
| Homepage / website | `https://github.com/REX-1320/Paiint` |
| License terms URL | `https://rex-1320.github.io/Paiint/docs/legal/EULA.html` |
| Privacy policy URL | `https://rex-1320.github.io/Paiint/docs/legal/PRIVACY.html` |
| Support contact | `support.paiint@gmail.com` |
| Age rating | done (all-ages badges issued) |
| System requirements | DirectX 12 min / rec; 4 GB min, 8 GB rec; keyboard+mouse; free text for CPU/GPU already supplied |
| Platform declarations | WMR: PC unticked; OneDrive backup unticked; "record and broadcast clips" unticked |

## Upload files

| Slot | File |
|---|---|
| Screenshots (16:9, PNG) | `C:\Users\Moinu\Downloads\Paiint Store Assets\demo\screenshot-1-library.png` (1920×1080) — lead |
| | `demo\screenshot-2-settings.png` — live engine telemetry |
| | `demo\screenshot-4-playlist-config.png` — playlist settings |
| | `demo\screenshot-3-about.png` — drop or last |
| Large logo 1080×1080 | `Paiint Store Assets\StoreLogo-1080x1080.png` |
| 2:1 logo 300×150 | `Paiint Store Assets\StoreLogo-300x150.png` |
| 1:1 logo 300×300 | `Paiint Store Assets\StoreLogo-300x300.png` |
| Package | `C:\Users\Moinu\Downloads\Paiint-0.1.0-x64.msix` — 183,062,897 B, SHA256 `1654A340A5C2B0D60EF2B1E105BCB76DCC163B188F6872324FA4B62E25DEF573` |

Do **not** upload: the older screenshots in the assets root (third-party artwork + personal paths), `demo\screenshot-5-desktop.png` (spare, bare gradient).

## Regenerate

- Logos: `python app/tools/make-store-logos.py --out <dir>` (fill 0.90 rule).
- Icon set: `python app/tools/make-icons.py`.
- Package: `powershell -File packaging\msix\build-msix.ps1 -Publisher "CN=9D8DC345-5B4D-4571-831E-2EBC542FFA47" -IdentityName "Moinu.Paiint" -PublisherDisplayName "Moinu"`.
