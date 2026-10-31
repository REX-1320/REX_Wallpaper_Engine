# Occlusion + diagnostics fixes (2026-10-31) — DONE

## Evidence

Two tester archives (`Downloads\Paiint Diagnostics_parkash.zip`, `Downloads\s_bhanu.rar` — misnamed, contains a `Paiint Diagnostics` folder) showed:

1. **Every sample was `{"error":"powerMonitor.getIdleTime is not a function"}`.** `collectDiagnosticsSample()` threw at `main.cjs:597`; Electron 43 has no `getIdleTime()` (it is `getSystemIdleTime()`), and the throw discarded the whole sample, so window/power/memory/audio fields were lost on every real machine.
2. **No `audio` events existed**, so a mute decision could not be audited.
3. **`FRAME_READY` flooded the JSONL** (~3.5k runtime events / 3 min, 1.2 MB / 3 min) — the 200 MB cap would be reached in hours.
4. **Crash recovery left empty session dirs and filed data under `recovered-<recovery-time>`** instead of the session that wrote it.
5. **Occlusion was foreground-only.** A visible window that is not focused (desktop clicked, overlay, second monitor) did not mute, reproducing "still plays on other app" even though Notepad muted.

## Tasks

- [x] 1. `collectDiagnosticsSample`: `powerMonitor.getSystemIdleTime()` + `sampleField()` per-section try/catch so one bad API cannot erase the whole sample.
- [x] 2. Recorder recovery: read the crashed session name from the head of `current.jsonl`, reuse its directory, remove stale empty session dirs; keep the `recovered-*` fallback.
- [x] 3. Drop `FRAME_READY` from diagnostics (renderer forwarding unchanged).
- [x] 4. Replace foreground-class occlusion with Z-order enumeration of visible top-level windows: skip invisible/minimized/cloaked/taskbar and anything under 0.5 % of a monitor; the desktop window terminates the scan (Win+D = clear).
- [x] 5. Log `audio` events when the mute decision changes and `occlusion` events when the state changes; samples carry the full decision.
- [x] 6. Tests: recorder recovery + empty-dir cleanup + malformed-record fallback, occlusion line parser, `desktopCovered` fallback/authority.
- [x] 7. Gates: `node --test` 124/124, lint 0, typecheck 0, `pnpm --dir app build` 0, live A/B.
- [x] 8. Live verification: Notepad/Chrome mute; Win+D → clear → audible → Win+D → occluded → muted; 120x16 `CHeadBarWindow` ignored; a 3.4 h session with 12,006 samples / 0 errors / `power.idleTime` present / 25 `audio` + 11 `occlusion` events / no `FRAME_READY`; crashed session recovered into its own dir with no new empty dirs.
- [x] 9. Tracking docs updated (`TODO.md`, `everything.md`, `#project_status.md`, spec); commit + push.
- [ ] 10. Rebuild `Paiint-Unpacked` + rar and the Store MSIX with this build.
