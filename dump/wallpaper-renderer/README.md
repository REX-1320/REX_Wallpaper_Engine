# wallpaper/renderer/ — archived orphan subsystem (2026-10-19)

Archived from `rex-core/src/wallpaper/renderer/` per the 2026-10-17 full-repo quality audit
(§5.6 #6/#7 + §6) and the user-approved Batch 4 hygiene plan.

**Why archived, not fixed:** this subsystem was absent from every CMake target — `rex-core/CMakeLists.txt`'s
`WALLPAPER_SOURCES` lists only `engine/ decoder/ playback/ audio/ video/ monitor/ loader/ scene/ runtime/`,
so these files never compiled into any binary. They reference a `WaitForExternalFence` that does not exist
anywhere in the codebase, and a repo-wide search confirms no live source or test includes any of these
headers. The live renderer is `rex-core/src/graphics/renderer/`.

**Latent bugs known at archive time (audit §5.6 #7 — do not trust this code as-is if revived):**

- `AudioVisualizerRenderer.cpp:161-162` — the fence value is never incremented.
- `RenderPipeline.cpp:171` — no `OMSetRenderTargets`; the RTV is unbound before draws.

**To revive:** add the directory to a target in `rex-core/CMakeLists.txt` (owner-gated edit), provide/replace
`WaitForExternalFence`, fix the two defects above, and re-check every compositor API it calls — audit §5.6
reports this copy as the last caller of several compositor APIs and it predates later compositor changes.

History is preserved: `git log --follow` on any file here traces back past the move.

Audit references: `docs/bugs/AUDIT-2026-10-17-quality-audit.md` §5.6 #6/#7 + §6.
