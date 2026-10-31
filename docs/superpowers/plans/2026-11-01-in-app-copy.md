# In-app copy (man-page minimal) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the generic in-app descriptions with the approved C-voice deck from the spec.

**Architecture:** String-only edits in three React files. No logic, layout, or component changes.

**Tech Stack:** React 19 + Vite (Electron shell).

**Spec:** `docs/superpowers/specs/2026-10-31-in-app-copy.md`

## Global Constraints

- Use the exact strings in the spec's deck. HDR stays in the About blurb.
- Do not change labels, buttons, dynamic status strings, empty states, or section titles.
- Do not touch the Store listing, `package.json`, or `AppxManifest.xml`.
- `pnpm lint`, `pnpm typecheck`, and `pnpm build` must stay green.

## Review Focus

- JSX attribute escaping: the diagnostics string keeps the existing `\\` in source so the UI still shows one backslash.
- No old string survives anywhere in `app/src`; grep proves it.
- The About blurb keeps the same visual length so the layout does not shift.
- The `=` fragments stay readable at 10.5 px and do not overflow the description slot.
- No test or snapshot asserts the old copy.

---

### Task 1: Apply the deck

**Files:**
- Modify: `app/src/App.jsx:23-33`
- Modify: `app/src/settings/SettingsPage.jsx` (descriptions at 145, 209, 215, 221, 232, 238, 249, 256, 262, 268, 282, 307; hint at 72)
- Modify: `app/src/about/AboutPage.jsx:45-77`

**Interfaces:**
- Consumes: the approved deck in the spec.
- Produces: no API changes; user-visible strings only.

- [ ] **Step 1: Replace the three page subtitles in `App.jsx`.**
- [ ] **Step 2: Replace the Settings row descriptions and the engine-offline hint in `SettingsPage.jsx`.**
- [ ] **Step 3: Replace the About tagline, blurb, privacy note, and FFmpeg note in `AboutPage.jsx`.**
- [ ] **Step 4: Prove the old strings are gone.**

Run: `rg -n "Your library, your playlists|How Paiint behaves|Live wallpaper engine|Start Paiint when you sign|Start in the tray without|Put the wallpaper back|Restore where you left|Stops the wallpaper and leaves|Off follows the monitor|Off uses the CPU|When Paiint quits, save|Mutes the wallpaper when the desktop|Cached preview images|How a newly imported|The wallpaper engine is not running" app/src`
Expected: no matches.

- [ ] **Step 5: Prove the new strings are present.**

Run: `rg -n "Library\. Playlists\. Desktop\.|Runtime configuration\.|Native wallpaper engine|No telemetry\. No network|Mute when covered|Freeze the final frame|Engine offline" app/src`
Expected: every new string appears once.

- [ ] **Step 6: Run the gates.**

Run: `pnpm lint`, `pnpm typecheck`, `pnpm build`
Expected: 0 errors.

- [ ] **Step 7: Update the tracking docs and commit.**

Add the copy change to `TODO.md` / `everything.md`, then commit the three source files plus the spec and plan.
