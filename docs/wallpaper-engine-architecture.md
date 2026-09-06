# REX Wallpaper Engine — Architecture Reference

> **Status: LOCKED (P15).** This document is the reference for all future
> wallpaper-engine work. Changes to anything marked **LOCKED** require an
> explicit design review and a versioned migration note in this file.

---

## 1. The 15 Non-Negotiable Rules

1. **The native runtime is the only wallpaper renderer.** Chromium never
   decodes wallpaper video; the UI shows static thumbnails only.
2. **The render thread must never block on the decoder.** Frame sync is a
   GPU-side fence wait (`ID3D12CommandQueue::Wait`); the SPSC scheduler is
   latest-wins and lock-free.
3. **No full-frame CPU buffer on the hardware path.** D3D11VA →
   VideoProcessorBlt → shared NT handle → `OpenSharedHandle` — zero copies.
4. **No per-frame resource creation.** Pool slots, SRVs, shared handles,
   command allocators/lists are created once (or once per resolution change)
   and reused. `allocatorsPerFrame` telemetry MUST stay flat at 0.
5. **Bounded surface pool: exactly `kPoolSize = 8` slots**, single source of
   truth in `FfmpegVideoDecoder.hpp`. Never grows with resolution count.
6. **One decoder feeds all monitors.** Identical wallpapers share the same
   decode via per-monitor compositors sampling one `VideoFrame`.
7. **Same-adapter guarantee.** The D3D11 decode device is created on the
   D3D12 renderer's adapter (LUID match). Shared NT handles are invalid
   across adapters; `decRendSameAdapter=1` is an invariant.
8. **Window thread owns all HWND mutations.** Only `DesktopWindowThread`
   creates/destroys/re-parents windows; other threads post requests.
9. **Verify before declaring recovery success.** `VerifyWallpaperLayer()`
   enforces all five layer invariants before any reacquire returns true.
10. **HDR follows the OS.** Swap-chain color space comes from
    `IDXGIOutput6::GetDesc1().ColorSpace`; SDR content on an HDR display
    tone-maps down, HDR on SDR tone-maps to 80-nit reference.
11. **Telemetry is always on.** 5s native sampler (`runtime-telemetry.log`),
    10s Electron sampler (`electron-telemetry.log`), GET_STATUS JSON —
    every field listed in §5 must keep working; they are regression alarms.
12. **Length-prefixed JSON IPC.** `[u32 LE][UTF-8 JSON]` on stdin/stdout.
    No framing changes without bumping a protocol version constant.
13. **Original media stays referenced, never copied.** Thumbnails live in
    the REX cache; metadata in app state; files stay where the user put them.
14. **Recovery preserves playback state.** Device loss / Explorer restart
    restores scene + position + playing state (`HandleDeviceLoss`).
15. **Failures abort loudly, not silently wrong.** Benchmark mode refuses
    unsupported codecs ("hardware decode unavailable"); capability-gated SET_*
    commands report skipped keys instead of no-oping.

---

## 2. Locked Decisions

| Area | Decision |
|---|---|
| Process model | Persistent `rex-wallpaper-runtime.exe --runtime`, spawned once by Electron |
| IPC | Length-prefixed JSON; 21 commands; events READY/LOADED/PLAYING/PAUSED/STOPPED/UNLOADED/ERROR/FRAME_READY |
| Decode | FFmpeg 8.x + D3D11VA primary, swscale fallback (software path only for codecs D3D11VA can't service) |
| Interop | D3D11 pool textures (`SHARED\|SHARED_NTHANDLE`) → NT handle → D3D12 open; monotonic slot fences |
| Scheduler | 8-slot SPSC ring, latest-wins, intermediates counted dropped |
| Command buffers | Main render list persistent; copy/upload paths use 4-entry context ring (P5) |
| Desktop host | WorkerW/Progman single-monitor + MultiMonitorDesktopHost; EDID-keyed persistence via SettingsService |
| Monitor identity | FNV-1a hash of EDID from PnP registry; survives reconnects |
| Recovery state machine | `DesktopHostState { Normal, Suspect, Repairing, Verified }`; render loop services both shell-restart and device-lost triggers |
| Color pipeline | Decode matrix → adjustments → ACES-fit tone-map → PQ-encode when output HDR; transfer enum 0=SDR 1=PQ 2=HLG |
| Telemetry | See §5 — format frozen; append-only logs under `%TEMP%\REX\` |

### Deferred (design space reserved, do NOT build casually)

Shader / Web / Scene / AudioVisualizer apply paths (renderers exist, entry
points don't), playlist IPC commands, cloud sync, plugin SDK.

---

## 3. Benchmark Matrix & Procedure

### Media prerequisite (BLOCKER for P13)

D3D11VA does **not** accelerate MPEG-4 ASP (`mp4v`). All clips produced by
`rex-gen-media.exe` are mp4v → benchmark aborts by design. Real clips are
required:

```
bench-media/matrix/
  h264_1080p30.mp4  h264_1080p60.mp4  h264_1440p60.mp4  h264_4k30.mp4  h264_4k60.mp4
  hevc_<same five>.mp4
  av1_<same five>.mp4        # AV1 hw decode needs RX 6700 XT+ / RTX 40+
```

Runner (portable, both machines):

```powershell
powershell -File rex-core\run-benchmark-matrix.ps1 -MachineTag amd6800
powershell -File rex-core\run-benchmark-matrix.ps1 -MachineTag rtx4050
```

Each case runs twice (default = hardware attempt, `--no-hw` = software) at
600 frames; captures console, `benchmark.log`, `runtime-telemetry.log`,
`crash.log` into `bench-results\<tag>\<clip>\<mode>\` plus `summary.csv`.

### Metrics compared across machines

Private RAM peak · VRAM dedicated/shared · decoder FPS · render FPS · frames
dropped · startup ms (open→first frame) · adapter-mismatch flag.

### AMD RX 6700 XT baseline recorded so far

| Case | Result |
|---|---|
| mp4v 1080p30 HW attempt | ABORTED (hardware decode unavailable) — correct behavior |
| mp4v SW fallback | startup 147 ms; benchmark refuses misleading run |
| Runtime soak 1080p30 mp4v (SW), 15 min | private 102→129.5 MB then flat; VRAM 335.8 MB const; **0 drops / 19,121 frames**; 21 fps steady |
| GET_STATUS topology | `decRendSameAdapter=true rendDispSameAdapter=true` LUID f956=f956 |

NVIDIA RTX 4050 column: **pending user run** — first divergence between the
two summary.csv files becomes the fix target.

---

## 4. Long-Run Soak Procedure (P14)

```powershell
powershell -File rex-core\soak-test.ps1 `
    -VideoPath <clip> -DurationSec 7200 -SampleEverySec 600
```

Samples every 10 min for 2 h into `.build\soak-log.csv`
(`time_s,privateMB,gpuDedMB,gpuSharedMB,gpuBudgetGB,decFps,rendFps,dropped,decoded,consumed`),
progress in `.build\soak-status.txt`.

**Pass criteria**

- Private RAM flat after warm-up (allocator arena steps OK; continuous
  linear growth = leak)
- VRAM dedicated constant ±small jitter (pool bounded)
- Zero or near-zero drops; `decoded ≈ consumed`
- No crash.log entries, no hang-watchdog dumps
- Exactly ONE runtime process (no accumulation)

Culprit mapping on failure: growing `gpuDed` → pool/descriptor leak;
growing private with flat VRAM → CPU-side frame leak; rendFps collapse →
fence stall; watchdog dump in crash.log → hung thread named inside.

---

## 5. Frozen Telemetry Fields

GET_STATUS `telemetry{}` — adding fields is allowed; removing/renaming is not:

`framesDecoded, framesConsumed, framesDropped, decoderFps, renderFps,
videoWidth, videoHeight, ringSlotCount, ringBytes, textureBytes, uploadBytes,
textureRecreations, uploads, allocatorsPerFrame, scenesLoaded, scenesReleased,
decodeWidth, decodeHeight, hdrActive, displayMaxNits, gpuDedicated, gpuShared,
gpuBudget, decRendSameAdapter, rendDispSameAdapter, decoderLuid, rendererLuid`

Log paths: `%TEMP%\REX\runtime-telemetry.log` (5 s),
`%TEMP%\REX\electron-telemetry.log` (10 s),
`benchmark.log`, `crash.log`, `runtime.log`, `cli.log`.

---

## 6. Known Limitations

1. **Test-media gap:** no local H264/HEVC/AV1 encoder (vcpkg ffmpeg built
   with encoders disabled); P13 blocked until real clips supplied.
2. MPEG-4 ASP runs software-only by codec limitation (correct, not a bug).
3. `rendererDisplaySameAdapter` requires monitor↔output name matching;
   reports false on headless/RDP sessions where outputs vanish.
4. Software video path still allocates a temp staging texture per upload
   (bounded, reused buffer exists; command contexts already pooled).
5. Per-monitor *different* videos would need a controller map — explicitly
   out of scope until playlist IPC lands.

## 7. Future Extensions (reserved seams)

Playlist commands over IPC (`PlaylistManager` compiled, undriven);
shader/web/scene LOAD types (renderers exist); thumbnail persistence bridge
(`thumbnail.js` + preload); tray/supervisor wiring (untracked modules ready);
V8 memory cap + `REX_PREFER_IGPU` gating.
