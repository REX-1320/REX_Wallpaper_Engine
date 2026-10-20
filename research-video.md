# REX — Video output research (2026-10-19)

> **What this is:** the commissioned answer to *"which is the best video output player (VLC is one of them) and everything about video decoding — and how do we confirm the file's elements (colors, light, HDR/SDR) == what the wallpaper shows?"*
> Four parallel, read-only research tracks (A players/renderers · B HDR→SDR tone mapping · C decoding & color pipeline · D verification + wallpaper-engine prior art), sourced from primary material: upstream source code (VLC, libplacebo, mpv, FFmpeg), ITU-R reports (BT.2100/2390/2408/2446), Microsoft Learn, vendor docs, Doom9/AVS/community consensus.
> **Context that triggered it:** on a 4K HEVC 10-bit DV-P8.1 clip, the engine's HDR→SDR path (PQ → ÷203 → BT.2020→709 → ACES filmic fit → sRGB) crushes shadows vs VLC 3.0.23 (27 % vs 9 % of pixels below code 4 on the dark test frame); the user reports "too much black" and treats VLC as the reference.
> Companion: `research.md` (2026-10-15 stage research: gallery/import, WorkerW, multi-monitor, recovery).

---

## 0. Headline verdicts

1. **VLC 3.x is good-but-dated, not the quality ceiling.** Its Windows D3D11 HDR→SDR path is *one* hard-coded Hable curve with a fixed 100-nit reference, no black-point handling, no metadata, no user controls (source-verified). It still beats the engine's ACES path, but it is not "the best".
2. **The quality leaders are madVR (frozen since 2018, HDR test builds only) and mpv/libplacebo (gpu-next)** — spline/BT.2390 curves, BT.1886 black-point compensation, peak detection, blue-noise dithering. **MPC Video Renderer** is the best free DirectShow option; **VLC 4.0** replaced the fixed shader with libplacebo (still unreleased as of Sep 2026).
3. **The standard fix for the "too much black" bug is the BT.2390 EETF** with explicit anchors: **SDR white 203 cd/m²** (BT.2408 HDR Reference White — what libplacebo/mpv use for SDR targets) and **source peak from MaxCLL/MDCV, fallback 1000 cd/m², never 10000**. The ACES filmic fit's toe (slope 0.214) is the direct cause of the shadow crush; hable's toe (0.383) is only a partial remedy.
4. **Matching VLC exactly is defensible as a compatibility preset, not as the default.** VLC's own project moved on (libplacebo), and "looks like VLC" cannot be arbitrated in a bug report; "BT.2390 + BT.2408" can.
5. **No wallpaper engine has a correct HDR→SDR story today** (Wallpaper Engine: "re-encode to BT.709"; Lively: mpv passthrough + default-bug history). Correct tone mapping is an open differentiator for REX.
6. **Verification:** build a trusted reference decode (FFmpeg CPU `zscale→tonemap→zscale`, or libplacebo GPU chain), then compare with a metric stack — **ΔEITP** for hue, **CAMBI** for banding, **XPSNR/VMAF** for overall, PSNR/SSIM only as smoke tests. Beware: `tonemap` default is `none`; missing output OETF is a classic false conclusion; dynamic peak detection breaks reproducibility.

---

## A. Players & renderers — who is actually best on Windows

### A.1 Taxonomy (2026-10)

| Player | Video path | HDR→SDR tone mapping | HDR display output | Status |
|---|---|---|---|---|
| **VLC 3.0.x** | own D3D11 / D3D9 / OpenGL vouts | fixed Hable curve in D3D11 shader | passes HDR metadata (D3D11) | Maintained (3.0.24, 2026-09-22); 4.0 unreleased |
| **mpv 0.41+** | **gpu-next = libplacebo** (Vulkan) by default | spline (default), BT.2390, dynamic peak, BPC, contrast recovery | passthrough + `--target-colorspace-hint` | Very active |
| **MPC-HC / MPC-BE + LAV** | DirectShow, renderer chosen | whatever the renderer does (madVR / MPC-VR) | via renderer | Actively maintained |
| **madVR** | own D3D11 renderer | reference dynamic tone mapping (per-scene measurement) | passthrough/convert | Public 0.92.17 frozen 2018-09-30; HDR test builds continue (madVRhdrMeasure 210, 2026-02); dev = commercial Envy |
| **MPC Video Renderer** | own D3D11 renderer | BT.2390 / ACES / Reinhard / Hable / Mobius + local TM; SDR nits default 125 | passthrough incl. DV features | Active |
| **PotPlayer** | own D3D11 | own HDR-correction shaders (community advises madVR) | passthrough | Closed source, adware history |
| **Kodi (Win)** | own GL/D3D | Reinhard/ACES/Hable options; pixel-shader path needed for best results | limited | Active |
| **JRiver (JRVR)** | **libplacebo** | mpv/libplacebo class | passthrough | Maintained, commercial |
| **Plex HTPC** | mpv engine (libmpv) | mpv defaults | passthrough | Maintained |
| Windows Media Player / Movies & TV | Media Foundation / EVR | none usable (WMP over-brightens HDR); Movies & TV = passthrough-oriented | decent passthrough | Legacy / maintained |

### A.2 Renderers

- **EVR/EVR-CP**: effectively abandoned for quality playback.
- **madVR**: still the DTM reference (per-frame measurement, target-nits semantics, highlight recovery, dynamic clipping rules); frozen public release, commercial successor (Envy).
- **MPC-VR**: the practical DirectShow successor; D3D11, DV support, BT2390 among curves.
- **libplacebo (Vulkan/OpenGL)**: the best maintained open stack — used by mpv gpu-next, JRiver JRVR, Plex; VLC 4 optional paths. **No D3D12 backend** — it is an oracle/precedent, not embeddable.
- **D3D11 native** (VLC, PotPlayer, MPC-VR): quality depends entirely on whose shader.
- **D3D12 (REX)**: no mature *player* renderer ships on it, but the API is not the problem — the math is.

Ranking of **tone-mapping engines** (engineering judgment from the surveyed evidence): **madVR ≳ libplacebo > MPC-VR > VLC 3.x D3D11 > Kodi/PotPlayer/apps > MF/EVR/WMP.**

### A.3 VLC deep dive (the bar to clear)

- `d3d_dynamic_shader.c` (master) / `d3d11_shaders.c` (3.0.x): exactly one curve — `hable(rgb * LuminanceScale) / hable(11.2)`; `hable`: A=0.15 B=0.50 C=0.10 D=0.20 E=0.02 F=0.30; no black-point compensation, no dithering step, no controls.
- `d3d_shaders.c`: `LuminanceScale = display.luminance_peak / D3D_GetFormatLuminance()`; PQ = 10000, HLG = 1000, SDR = 100 → **0.01 on an SDR display** (matches our per-pixel measurement of VLC 3.0.23).
- SDR encode: `pow(rgb, 1/2.2)` — a *pure 2.2 power law*, not the sRGB piecewise curve the Windows desktop expects (worth ≈8 codes in deep shadows).
- `d3d11_tonemap.cpp` (© 2024) is **SDR→HDR** (NVIDIA True HDR) — *not* a better HDR→SDR path; VLC 4.0 hasn't shipped. "Wait for VLC 4" is not a fix.
- Historical quotes: mpv #6993 ("VLC does probably the best job in hdr tone mapping", later edited), mpv #9800 frames madVR as the target. VLC is praised for *plug-and-play*, not for being the maximum-quality reference.

### A.4 What the leaders have that REX lacks

| Feature | libplacebo / mpv | VLC 3.x D3D11 | REX today |
|---|---|---|---|
| Curve | spline default; bt.2390/bt.2446/st2094 available | Hable only (legacy-quality per libplacebo docs) | ACES filmic only |
| Black point | bt.1886 BPC default; `black_cutoff` ≈1 % PQ | none | none → crush |
| Peak handling | histogram peak detection (`--hdr-compute-peak`), scene-aware | fixed 100-nit reference | fixed |
| Contrast recovery | high-quality preset 0.3 | none | none |
| Dithering | blue-noise on by default | none in the shader | none (verified: no dither in engine shader) |
| Controls | many (`--tone-mapping*`, `--hdr-*`, `--target-*`) | none on D3D11 | app-level only |
| DV | RPU reshaping (P8 base layer) | falls back to HDR10 base layer | treats as PQ (correct for P8.1) |

---

## B. HDR→SDR tone mapping — standards and implementations

### B.1 Fundamentals

- **PQ (ST 2084)** is absolute display-referred: code = luminance in cd/m² up to 10000. 1 cd/m² ≈ 15 % of full code; 100 → 51 %; 1000 → 75 %.
- **HLG** is scene-referred; display light requires the **OOTF** on luminance (γ = 1.2 at 1000 cd/m²; γ = 1.2 + 0.42·log10(LW/1000) generally) — never feed HLG through a per-channel curve without it.
- **BT.2408 reference levels:** HDR Reference White (diffuse white / graphics white) = **203 cd/m² = 58 % PQ = 75 % HLG**; 18 % grey card = 26 cd/m²; SDR nominal peak = 100 cd/m² (some workflows use 203). Mapping 203 → SDR white is a deliberate design choice in every converter.

### B.2 The BT.2390 reference EETF (the standard static HDR→SDR mapping)

In normalized PQ domain (`x` = PQ code of the pixel; `maxLum` = PQ of the target peak; knee `KS = 1.5·maxLum − 0.5` — spec value):

```
if x ≤ KS:  E2 = x                                  // identity below the knee
else:       t  = (x − KS)/(1 − KS)
            E2 = (2t³ − 3t² + 1)·KS + (t³ − 2t² + t)·(1 − KS) + (−2t³ + 3t²)·maxLum
E3 = E2 + b·(1 − E2)^4                              // optional black lift (PLUGE)
```

Monotonic, slope-1 at the knee, hits `(1, maxLum)`. Implementations deviate knowingly:
- **libplacebo** `pl_tone_map_bt2390`: knee offset default **1.0** instead of the spec's 0.5 (`--tone-mapping-param=0.5` restores the spec curve); normalizes to the source's *actual* peak; adds generalized black-point adaptation.
- **mpv**: `--target-peak=auto` → **203 cd/m² for SDR TRC**; `--hdr-reference-white=auto` queries the *OS* on Windows; gpu-next default curve = **spline** (single-pivot hyperbolic, pivot from scene average → semi-dynamic); legacy `gpu` default = bt.2390.
- **MPC-VR** offers BT2390 among its curves (0.10.1+, 2026-04); **madVR** exposes it and drives targets from measured display capability.

### B.3 The other standardized mappings

- **BT.2446 Method A** (static, well-graded content): 1000→100 cd/m², anchored so 203 cd/m² HDR lands at **90 % of the SDR signal**; libplacebo `bt2446a` (+inverse). **Method B** display-referred; **Method C** parametric (~95 % anchor).
- **Dolby**: per-shot RPU dynamic metadata (L1 min/mid/max + trims); P8.1 = HDR10 base + RPU; **on Windows there is no OS DV passthrough** → HDR10 base layer is the realistic target; Dolby grades the SDR trim at **100 cd/m² Rec.709 white**.
- **Netflix/streaming**: static metadata as floor, HDR10+ dynamic metadata as differentiator (preserves intent in dark *and* bright scenes).

### B.4 Curve comparison + worked numbers (8-bit sRGB codes, same stimuli)

| Curve | Toe behaviour | Anchor | Notes |
|---|---|---|---|
| ACES filmic fit (**current engine**), x = L/203 | slope **0.214**; f(0)=0 | hits 1.0 at ≈1470 cd/m²; asymptote 1.033 | no knee, no black lift → **crush** |
| Hable (VLC 3.0), ÷hable(11.2) | raw slope 0.278 → normalized **0.383** | **1120 cd/m² → 255** | clips above 1.12 k cd/m²; no metadata |
| **BT.2390 EETF** | **slope 1 (identity) below knee** (~88 cd/m² in the recommended config) | `maxLum` by construction | spec-clean, monotonic |
| BT.2446A | log segment then BT.1886 | 1000→100 cd/m², 58 % PQ → 90 % signal | "recommended for well-mastered content" (mpv) |
| spline (mpv default) | hyperbolic toe | dst peak | semi-dynamic (scene average) |

Stimulus → code (VLC 3.0 | engine today | BT.2390 → 203 white, source peak 1000):

| cd/m² | 0.1 | 1 | 5 | 10 | 26 | 50 | 100 | 203 | 400 | 1000 |
|---|---|---|---|---|---|---|---|---|---|---|
| VLC 3.0 | 7 | **20** | 42 | 58 | 88 | 115 | **149** | **186** | 219 | 252 |
| Engine today | 0 | **5** | 32 | 59 | 117 | 164 | **205** | **232** | 245 | 253 |
| BT.2390 → 203 | 2 | **15** | 44 | 59 | 100 | 136 | **186** | **229** | 249 | 255* |

(\* clipped only because the assumed source peak is 1000; with MaxCLL=4000 it stretches instead.)

Reading: (1) the engine crushes deep shadow relative to both references (code 5 vs 20 vs 15 at 1 cd/m²; "code 4" ≈ 1.15 cd/m² for the engine vs 0.03 VLC / 0.24 EETF → exactly the 27 % vs 9 % histogram); (2) it is **brighter in the mid/high midtones** (205 vs 149 vs 186 at 100 cd/m²) and reaches near-white sooner → HDR wallpapers look washed-out next to SDR content; (3) VLC's shadow brightness is ~⅔ curve shape, ~⅓ the pure-2.2 encode.

### B.5 Recommendation (research verdict)

1. Keep the pipeline skeleton (PQ/HLG → cd/m² → BT.2020→709 → tone map → encode).
2. **Replace the ACES fit with the BT.2390 EETF** (evaluated inline or via 1024-entry LUT; cheaper than the current rational function).
3. **Make the anchors explicit/configurable:** `sdr_white_nits` default **203** (BT.2408; matches libplacebo `PL_COLOR_SDR_WHITE` and mpv); `source_peak_nits` from MaxCLL/MDCV else **1000** (never 10000).
4. **Output encode:** sRGB piecewise for the desktop; keep `pow(x,1/2.2)` only inside a **"VLC-compatible" preset** (exact `hable(0.01·L)/hable(11.2)` + 2.2), explicitly labelled.
5. HLG: inverse OOTF first, then the same PQ-domain mapper. Gamut: soft desaturation (mpv `perceptual`/FFmpeg `desat` style) as a follow-up — not blocking.
6. Optional later: cheap dynamic mode (downsampled histogram peak, low-passed; mpv `--hdr-compute-peak` semantics). Static metadata + EETF gets most of the benefit for none of the risk.
7. Acceptance test for the fix: 1 cd/m² ∈ [15, 21] codes; 203 cd/m² ∈ [180, 210]; monotonic; no clipping below `source_peak`; dark-frame histogram moves 27 % → ~10–15 % below code 4.
8. **Is "match VLC" defensible?** Yes as a preset (deterministic, reverse-engineered exactly, user's trusted reference); no as the default (one fixed curve, fixed 1120-nit anchor, per-channel, gamma-mismatch artifact, superseded upstream). A product arbitrates bugs against a standard, not a player's look.

---

## C. Decoding & color pipeline — container to display (Windows)

### C.1 Codecs / Dolby Vision

| Codec | Profiles | HW decode on Windows | Notes |
|---|---|---|---|
| H.264 | High / High10 | everywhere | High10 often SW-only on old GPUs |
| HEVC | Main, **Main10**, Main12/4:4:4 | Main+Main10 widespread; Main12 spotty | Main10 = HDR workhorse |
| VP9 | P0, P2 (10-bit) | inconsistent older GPUs | YouTube HDR legacy |
| AV1 | Main | RTX 30+/Arc/RDNA2+ | dav1d = SW reference; film-grain synthesis differs HW vs SW |
| VVC | — | none consumer | ignore |

- **DV P8.1 (compat id 1) = HDR10 base layer + RPU.** Ignoring the RPU yields **correct plain HDR10** — the right Windows behavior, and what REX does. **P5 (IPTPQc2) has no compatible base** — without DV rendering it looks like garbage; detect and refuse. P7 = BL+EL+RPU (Blu-ray; FEL skippable to P8.1).
- FFmpeg plumbing: `dvcC`/`dvvC` boxes → `AV_FRAME_DATA_DOVI_METADATA` / `_RPU_BUFFER`; `libplacebo` consumes RPU for TM (can't re-encode DV).

### C.2 Hardware decode

- **APIs**: D3D11VA (mainstream; array of NV12/P010/P016 textures), DXVA2 (legacy), Media Foundation MFTs (what OS apps use), **D3D12 Video Decode** (natural zero-copy for REX; FFmpeg `d3d12va`), vendor APIs (NVDEC/QSV/AMF).
- **Zero-copy vs copy-back**: zero-copy is cheaper but driver-defined formats/layouts and hard to debug; **copy-back is the correctness baseline** (one extra copy is negligible at wallpaper scale), zero-copy only after per-GPU format/range verification with a runtime fallback. Recommendation ladder: HW zero-copy (verified) → HW copy-back → SW (avcodec/dav1d).
- **Format ≠ transfer**: P010 is *not* HDR (10-bit SDR lives there too). Branch HDR on `color_trc` (PQ/HLG vs bt709), never pix_fmt. (Unit 18 already enforced this in the engine.)
- **Driver quirks**: NVIDIA HEVC Main12 decodes to P010 only (drops 2 bits); Intel returns P016; AMD + P010 caused MPC-HC black screens; HW ≠ SW bit-exact by design. Hybrid GPUs: decode on the adapter driving the output, else SW.

### C.3 Containers & metadata

- MKV `Colour` (matrix/transfer/primaries/range) + `MasteringMetadata` + `MaxCLL`/`MaxFALL`; MP4 `colr` (`nclx`)/`mdcv`/`clli` (parser treats mdcv/clli as authoritative over VUI); `ffprobe -show_streams -show_frames` + MediaInfo cross-check.
- WEB-DL pitfalls: missing mastering data (need a documented fallback), wrong range flags (washed/crushed), BT.2020-primaries + BT.709-matrix mixes, DV metadata stripped (P8.1 → plain HDR10, fine), HLG (`arib-std-b67`) vs PQ (`smpte2084`) confusion. **Our test clip is exactly the "no mastering metadata" case** → fallback 1000-nit source peak is the documented choice.

### C.4 Pixel/color chain

- Matrices BT.601/709/2020 (NCL forms; CL rarely signaled), limited vs full range (double-stretch vs never-expanded bugs), **chroma siting** (MPEG-2/AVC left; HEVC BT.2020 top-left) — wrong siting = half-pixel color shift; chroma upsampling quality: nearest → jagged, bilinear acceptable, spline/catmull + chroma-aware preferred.
- **10→8-bit must dither** (ordered or error diffusion); truncation → banding (measure with CAMBI). Transfer-function bugs: PQ-as-gamma, double gamma, missing OETF.
- **Windows output**: SDR = `R8G8B8A8_UNORM` + `RGB_FULL_G22_NONE_P709`; HDR10 = `R10G10B10A2_UNORM` + `RGB_FULL_G2084_NONE_P2020`; scRGB = FP16 + `RGB_FULL_G10_NONE_P709` (**1.0 = 80 nits**). `SetHDRMetaData` is documented "no longer recommended" → tune-map to the display's real `MaxLuminance` (`IDXGIOutput6::GetDesc1`). SDR-on-HDR desktop: DWM composites SDR apps with the sRGB piecewise curve at the user's SDR-white level (readable via `DISPLAYCONFIG_SDR_WHITE_LEVEL`) — the "washed-out with HDR on" complaint; render in the desktop's actual mode and let DWM handle SDR.
- Artifact catalog (symptom → cause): washed-out (range expanded twice / P010 presumed HDR / SDR-in-HDR level), crushed blacks (double range, PQ-as-2.2, clip instead of EETF), banding (no dither, nearest chroma, 8-bit), green/pink (P010↔NV12 mismatch, DV P5, bad matrix, driver bugs), flat gray HDR (no TM), judder (23.976 on 60 Hz), macroblocking, hue shifts (matrix/primaries mix).

### C.5 Verification tooling

`ffprobe` + MediaInfo cross-check; **DXVA Checker** for per-GPU profiles/formats; Task Manager "Video Decode" engine / FFmpeg verbose log to confirm HW decode; HW-vs-SW `rawvideo` dump + PSNR/VMAF diff (small diffs normal; large = format/range bug); mode logging (`GetDesc1` colorspace/luminance + SDR white) to correlate artifacts with HDR toggles.

---

## D. Verification — "does the wallpaper == the file?" — and wallpaper-engine prior art

### D.1 What "correct" can mean (use all three deliberately)

1. **The file + a trusted decode chain** — the *numerical* reference.
2. **A reference player's output on the same machine** (VLC/mpv) — the *subjective* reference ("looks right"), not automatically correct.
3. **The standards** (BT.2390/BT.2408/EBU practice) — the *policy* for defaults; deterministic and defensible.

For SDR content on an SDR desktop, equality is definable and testable (identity chain). For HDR content, equality to the file is *undefined* until a tone map + anchors are chosen — the deliverable is "this documented mapping, verifiably", not "identical to the file".

### D.2 Metric stack

| Metric | Catches | Notes |
|---|---|---|
| PSNR | gross errors (range, channel swap, matrix) | smoke test only; perceptually weak |
| SSIM / MS-SSIM | structural loss | luminance-insensitive — wrong-brightness can score 0.99 |
| VMAF | overall perceived quality | SDR-trained; use as relative/delta; 6 points ≈ 1 JND |
| **XPSNR** | psychovisually weighted PSNR | FFmpeg 7.1+; report min plane/frame average |
| **CAMBI** | **banding/contouring** | 0–24, ≈5 = slightly annoying; use `full_ref=true`; the metric for the shadow-crush class |
| **ΔEITP** (BT.2124) | hue/color deviations in HDR | ~1.0 = 1 JND; watch the 95th percentile |
| TMQI | tone-mapped naturalness (no SDR truth) | compare two tone maps of the same HDR source |

Harness rules: identical resolution/format both sides; `settb/setpts` reset; **input order differs per filter** (`psnr`/`ssim`: test,ref; `libvmaf`: distorted,ref; `xpsnr`: ref,distorted); lossless intermediates; per-frame stats + worst-case pooling; any intentional look change updates baselines in the same commit.

### D.3 Reference HDR→SDR chains

- CPU (docs-blessed): `zscale=t=linear,tonemap=...,zscale=t=bt709:m=bt709:r=tv` — **`tonemap` default is `none`** (silent trap); peaks in units of 100 nits (`peak=10` = 1000); `npl` default 100 cd/m²; set `desat=0` for metric purity.
- GPU (closest to mpv): `libplacebo=tonemapping=bt.2390:colorspace=bt709:...:peak_detect=false:format=yuv420p` — pin `tonemapping` (auto), disable peak detection for reproducibility, dithering stays on; `apply_dolbyvision` on by default.
- Pitfall ranking: missing output OETF (washed-out), double transfer (crushed), tonemap=none, wrong npl, missing tags, range confusion, DV P5, determinism (dynamic peak / HW≠SW), circular references ("the app under test is the reference"), build variance (record ffmpeg/driver/OS-HDR state).

### D.4 Test patterns & end-to-end practice

Mehanik HDR10 set, AVSHD 709, Spears & Munsil 2023 (tone-map torture tests), DVS HDR-10, EBU TR 081. Minimal CI set: 10-bit gradient ramp (highest value), near-black PLUGE, color bars, tagged with `setparams`. Frame-exact alignment via burned-in counter or best-PSNR offset search; symmetric-transform tests (YUV↔RGB identity, 10→8 dither check) before blaming tone mapping. **Capture trap:** under an HDR desktop Desktop Duplication returns FP16 scRGB (1.0≈80 nits) with poor implicit SDR conversion — prefer offscreen readback; PrintWindow is acceptable for SDR windows; treat desktop capture as approximate.

### D.5 Wallpaper-engine prior art

| Engine | Video path | HDR story |
|---|---|---|
| Wallpaper Engine | own renderer; 2026: opt-in DX11 "HDR Support" video framework | none documented before 2026; official advice = "reset GPU driver settings / re-encode to BT.709" |
| Lively Wallpaper | **mpv gpu-next/libplacebo** since 2.2 (Sep 2025) | HDR passthrough (inherits libplacebo TM); bug history (SDR video wrong on HDR screen #1482, SDR→HDR "deepfried" #2800) shows *defaults* are the hard part |
| Plastuer | Chromium | no HDR story |
| DreamScene (historic) | WMV/MPG | predates HDR |
| Windows 11 native video wallpaper | hidden Insider builds only | undocumented, unshipped |

Lessons: the market's answer to HDR→SDR has been "don't process"; deterministic, well-tagged defaults + visible diagnostics beat smart opaque auto-detectors; "pass through" (HDR desktop) and "map down" (SDR desktop) are two different products.

### D.6 Decision framework

- **A. Match a player** (parity mode): matches enthusiast expectations, but their defaults churn and you inherit their bugs.
- **B. Follow standards** (BT.2390 + BT.2408, deterministic): stable, testable, defensible — right default for a product.
- **C. Expose choice:** only useful if presets are honest and verifiable.
- **Recommended hybrid:** standards default + a "VLC-compatible" preset for A/B + a "most accurate" diagnostic mode (`clip` TM + `clip` gamut) + controls named after familiar options (`tonemapping`, `target-peak`, `peak-detect`) + CI gates on the metric stack.

### D.7 SDR target-brightness data points

BT.2408 reference white **203 cd/m²**; Windows SDR-content-brightness slider ≈80 nits (0) → ~480 (100), readable exactly via `DISPLAYCONFIG_SDR_WHITE_LEVEL` (nits = value/1000 × 80); RTINGS "good" SDR real-scene ≈300 cd/m² monitors / ≈500 TVs; mpv/libplacebo internal SDR white = 203. Rule: default the tone-map target to the *actual* OS SDR white level, fall back to ~203, user override; keep dynamic peak detection off for wallpaper use.

---

## E. Synthesis for REX (the plan)

**Bug:** HDR→SDR shadow crush / "too much black" — root cause: ACES filmic toe (0.214) + ÷203 + sRGB encode (validated numerically against VLC and against a VLC-model reimplementation).

**Fix design (unit 20):**
- Default: **BT.2390 EETF** in the PQ domain, `sdr_white_nits = 203`, `source_peak_nits = maxCLL/MDCV else 1000`; sRGB piecewise output encode; keep identity below the knee.
- Preset: **"VLC-compatible"** = exact VLC 3.0 math (`hable(0.01·L)/hable(11.2)`, `pow(x,1/2.2)`) — the draft shader edit already implements this path; promotable as the compat mode.
- HLG: inverse OOTF before the mapper (already partly present).
- Acceptance: 1 cd/m² ∈ [15,21]; 203 ∈ [180,210]; monotonic; dark-frame histogram 27 % → ~10–15 % below code 4; live capture vs VLC on the Spider-Man test frame (t=180 s).

**New backlog surfaced by the research (now in `TODO.md`):** output dithering on the 10→8-bit SDR path; DV P5 detect-and-refuse; chroma upsampling quality; OS SDR-white query for HDR-display mode; verification harness (CAMBI/ΔEITP + reference chains) into `rex-core/tools/color/`.

**Explicitly not doing:** embedding libplacebo (no D3D12 backend; use as oracle), DV RPU rendering, dynamic peak detection for wallpapers (static content, risky), matching a player as the default.

---

## F. Sources (the load-bearing ones)

**Source code (verified)**
- VLC 3.0.x: `modules/video_output/win32/d3d11_shaders.c`, `d3d11_quad.c`, `d3d_shaders.h` — https://github.com/videolan/vlc/tree/3.0.x/modules/video_output/win32
- VLC master: `d3d_dynamic_shader.c`, `direct3d11.cpp`, `d3d11_tonemap.cpp`, libplacebo glue `modules/video_output/libplacebo/utils.c` — https://github.com/videolan/vlc
- libplacebo `src/tone_mapping.c`, `PL_COLOR_SDR_WHITE 203.0` — https://github.com/haasn/libplacebo ; options: https://libplacebo.org/options/
- mpv `MP_REF_WHITE 203.0`, `--target-peak`, `--hdr-compute-peak` — https://mpv.io/manual/master/ ; GPU-Next wiki: https://github.com/mpv-player/mpv/wiki/GPU-Next-vs-GPU
- FFmpeg `tonemap`/`zscale`/`libplacebo`/`psnr`/`xpsnr`/`libvmaf` docs; `vf_tonemap.c` (`REFERENCE_WHITE 100.0`), `colorspace.c` (`ff_determine_signal_peak`) — https://ffmpeg.org/ffmpeg-filters.html
- MPC Video Renderer — https://github.com/Aleksoid1978/VideoRenderer ; MPC-HC FAQ #2869 — https://github.com/clsid2/mpc-hc/issues/2869
- Netflix CAMBI doc — https://github.com/Netflix/vmaf/blob/master/resource/doc/cambi.md ; XPSNR — https://github.com/fraunhoferhhi/xpsnr

**Standards**
- ITU-R BT.2100-3 (PQ/HLG) — https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.2100-3-202502-I!!PDF-E.pdf
- ITU-R BT.2390-8/-12 (EETF) — https://www.itu.int/dms_pub/itu-r/opb/rep/R-REP-BT.2390-8-2020-PDF-E.pdf
- ITU-R BT.2408-9 (2026) (reference levels) — https://www.itu.int/dms_pub/itu-r/opb/rep/R-REP-BT.2408-9-2026-PDF-E.pdf
- ITU-R BT.2446-1 (methods A/B/C) — https://www.itu.int/dms_pub/itu-r/opb/rep/R-REP-BT.2446-1-2021-PDF-E.pdf
- ITU-R BT.2124 (ΔEITP) — https://www.itu.int/rec/R-REC-BT.2124/en
- Dolby Vision grading best practices — https://professional.dolby.com/siteassets/content-creation/dolby-vision-for-content-creators/dolby_vision_color-grading_best-practices_v4.2.pdf

**Microsoft / platform**
- HDR + DirectX — https://learn.microsoft.com/en-us/windows/win32/direct3darticles/high-dynamic-range ; using DirectX with Advanced Color — https://learn.microsoft.com/en-us/windows/win32/direct3darticles/using-directx-with-advanced-color
- `DXGI_OUTPUT_DESC1` / SDR white level — https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_6/ns-dxgi1_6-dxgi_output_desc1 ; `DISPLAYCONFIG_SDR_WHITE_LEVEL` — https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-displayconfig_sdr_white_level
- `SetHDRMetaData` ("no longer recommended") — https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_5/nf-dxgi1_5-idxgiswapchain4-sethdrmetadata ; 10/16-bit YUV formats — https://learn.microsoft.com/en-us/windows/win32/medfound/10-bit-and-16-bit-yuv-video-formats
- ACM — https://devblogs.microsoft.com/directx/auto-color-management/ ; Auto HDR — https://devblogs.microsoft.com/directx/auto-hdr-preview-for-pc-available-today/

**Community / competitors**
- madVR + madMeasureHDR thread — https://www.avsforum.com/threads/madvr-tool-madmeasurehdr-optimizer-measurements-dynamic-clipping-target-nits.3040072/
- mpv issues #6993, #7984, #9800, #10122 — https://github.com/mpv-player/mpv/issues
- Wallpaper Engine help (videos/artifacts) — https://help.wallpaperengine.io/en/videos/artifacts.html ; Steam discussion — https://steamcommunity.com/app/431960/discussions/2/3194737800642362069
- Lively changelog + issues #1482/#1925/#2800 — https://livelywallpaper.net/changelog
- r/htpc HDR guide — https://r-htpc.github.io/wiki/hdr

*Track detail: the four full research notes (A players 226 lines, B tone mapping 485, C decoding 327, D verification 393) were authored in a scratch directory; this file is the distilled, decision-focused record. Numbers labelled "⚠ community claim" in the notes were left out here unless corroborated by source/docs.*
