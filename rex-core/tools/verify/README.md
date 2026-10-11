# tools/verify — playback/telemetry/integration harnesses

Rescued 2026-10-10 from `%TEMP%\opencode\rex-verify\` (wipe-prone). All paths
below assume the repo at `D:\REX_doc` — edit the constants on top of each
script when reusing elsewhere. Raw per-second logs live in `fps.md`.

## FPS / telemetry (P0 clock plan + FPS audit + Ralph loop)

- `fps_log.cjs <exe> <media> <title> <cap> <outfile> <secs> [loop]` —
  per-second `title | src | dec | rend | out | eff | drop | rep | wrap |
  cons | decc | mons` log over the frame protocol. Truncates outfile first.
- `run_one.ps1 -Exe -Media -Title -Cap -Dir -Secs -Loop` — one video run:
  layer-busy preflight, node logger, PID capture, foreground typeperf
  sysmon (wildcard process + GPU-engine counters, `-y`), shortfall guard.
- `run_batch.ps1 -List <csv>` — batch driver over `batchC.csv`
  (`media,title,cap,tag`).
- `summ_all.ps1` — aggregate fps.txt per video (dec/rend/out avgs, drops/reps).
- `summ_sys.ps1 -Dir` — aggregate sys.csv PID-matched (cpu/ws/priv/io +
  per-engtype GPU 3D/Video/Copy via `rtpid.txt`).
- `matrix_p01.cjs` — P0.1 live transition matrix (video→image→video→STOP)
  with render-window visibility checks.
- `load_probe.cjs <media> [play] [sw]` (+`REX_PROBE_EXE` env) — minimal
  INIT/LOAD/PLAY/GET_STATUS probe with raw protocol visibility.
- `raw_probe.cjs` — raw frame bytes check (framing debugger).
- `status_1s.cjs` — one-shot GET_STATUS printer.
- `winvis.ps1` — reports `REXRenderWindow` HWNDs + `IsWindowVisible`.
- `steady120.ps1` — steady-state window analysis over `fps.md` v5 logs.
- `analyze_v4.ps1` — cumulative-delta (true unique/decode rate) analysis.
- `durations.ps1` — ffprobe durations (loop-wrap alignment).
- `engtypes.ps1` — lists live `\GPU Engine` engtype values for counter design.
- `chk_sys.ps1` — sys.csv sanity (blank-instance detector).

## Integration proofs (corrected recovery plan)

- `sup_wire.cjs` + `fake_native.cjs` — P0.2 headless proof (12 scenarios:
  exit→restore, park, quit-block, renderer guard, hung path, parked beat).
- `p03_wire.cjs` — P0.3 headless proof (20 checks: app-state round-trip,
  backups, corrupt recovery, library dedup/persistence, preload channels).

## Forensics / environment

- `crashlist.ps1`, `crashlog.ps1`, `wercheck.ps1` — WER Application-log
  fault summaries (faulting module + exception code timelines).
- `lockcheck.ps1`, `whoowns.ps1`, `whoparents.ps1`, `killrt.ps1` —
  exe-lock + orphan-runtime ownership/kill toolkit.
- `lintcheck.ps1`, `build_check.ps1`, `probe_direct.ps1` — convenience runners.

## Conventions learned the hard way (read before extending)

- PowerShell 5.1: no `&&`, no `head`/`grep`/`printf`; `>` is UTF-16
  (use `Out-File -Encoding utf8`); `$` vars need script files, not inline.
- typeperf needs `-y` for overwrites; bare `\Process(name)` can return
  blanks — use wildcards + `ID Process` PID matching.
- `Start-Process` splits args at spaces — quote exe/media.
- `fps_log.cjs` argv: `[2]=exe [3]=media [4]=title [5]=cap [6]=outfile
  [7]=secs [8]=loop`. The secs-index bug (argv 6 vs 7) cost a full day —
  see fps.md harness notes.
