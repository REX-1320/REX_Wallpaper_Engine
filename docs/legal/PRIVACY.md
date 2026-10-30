# Paiint Privacy Policy

**Version 1.0 — 26 October 2026**
Copyright © 2026 MoiNeX. All rights reserved.

Paiint is a desktop wallpaper engine. It runs entirely on your PC. **It does
not collect personal data, and it does not send anything anywhere.**

## 1. What Paiint does not do

- No account, sign-in or registration.
- No telemetry, analytics, usage statistics or advertising identifiers.
- No crash or diagnostic upload. Crash reports and logs are written to your
  own disk and stay there.
- No network requests from the application. It does not check for updates
  itself, phone home, or contact any server we run.
- No access to your files beyond the wallpapers and folders you choose.

## 2. What Paiint stores on your PC

All of it is local, and all of it is yours:

| What | Where | Why |
|---|---|---|
| Wallpaper library, playlists, presets and app settings | `%APPDATA%\Paiint` | Remember what you imported and how you configured it |
| Thumbnail cache | `%APPDATA%\Paiint\thumbnails` | Show gallery posters without re-decoding every file |
| Last frame left on the desktop at quit | `%USERPROFILE%\Pictures\Paiint\last-frame.jpg` | Keep the final video frame as a still wallpaper |
| Runtime logs and crash reports | `%TEMP%\Paiint` | Let you (or us, if you send them) diagnose a problem |
| Test-mode diagnostics (optional) | `%USERPROFILE%\Downloads\Paiint Diagnostics` | Only when you turn on Settings → Advanced → Test mode diagnostics (or launch with `--diagnostics`); records local engine and device details so you can study or report a problem. Nothing is uploaded. |

Test-mode recordings are ordinary files on your PC. Paiint has no code that
uploads them; you choose whether to share them.

Deleting the app does not delete these folders; you can remove them at any
time. The Settings page shows the exact paths and can clear the thumbnail
cache.

## 3. Your media

Paiint opens the images and videos you select in order to draw them on your
desktop. It does not copy, upload, share or analyse them. The only file it
writes from your media is the optional last-frame snapshot described above,
which stays on your PC.

## 4. Microsoft Store

If you install Paiint from the Microsoft Store, Microsoft processes the
purchase, installation and update data needed to deliver the app, under
Microsoft's own privacy statement. We receive only the aggregate sales and
rating information the Store makes available to every publisher — never your
identity or your files. A free acquisition still goes through the Store.

## 5. Third-party components

Paiint bundles FFmpeg (LGPL-2.1) and other libraries to decode media. They run
locally inside the app and make no network connections. See
`THIRD_PARTY_NOTICES.md` for the full list and licence texts.

## 6. Children

Paiint is a general-purpose desktop utility. It is not directed at children
and collects no data from anyone, including children.

## 7. Your rights

Because we hold no personal data about you, there is nothing for us to export,
correct or delete. If you believe we hold something, contact us and we will
check. You can remove every trace of Paiint from your PC by uninstalling it and
deleting the folders listed in section 2.

## 8. Changes

If a future version ever needs to collect something, this policy will be
updated before that version ships, the change will be described in the release
notes, and it will be opt-in where the law requires consent.

## 9. Contact

MoiNeX — `support@paiint.app`
