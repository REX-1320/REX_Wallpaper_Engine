# tools/color — colour-accuracy verification scripts

Rescued 2026-10-10 from `%TEMP%\opencode\rex-verify\` (wipe-prone).

## The mpv-parity set (2026-10-20)

The engine's HDR→SDR path is now a port of **libplacebo v7.360.0** (the build
in mpv 0.41.0), so these three pieces check the same thing the engine does:

- `shader_port.py` — a Python mirror of the engine's tone path: the
  libplacebo spline curve (solved from a measured peak/average), the IPT
  hue-preserving tone map and the black-scaled output. Run it to print the
  24-band ramp next to **mpv's own measured output**:
  `python shader_port.py`
- `parity_check.py` — measures a screenshot of the wallpaper (or of mpv's
  window) and compares the band codes with the model:
  `python parity_check.py capture.png [row_y]`
  It needs no third-party packages (the PNG reader is built in) and exits
  non-zero when the capture drifts from the model.
- `make_bands.py` — generates the known-luminance band test clip. For the
  mpv-parity work the ramp is the 24-band PQ clip (0.01 … 4000 cd/m²) with
  `bt2020nc/smpte2084` tags; the older 8-band `hdr_bands.mp4` is still here
  for the ITU-R BT.2408 checks.

**Reproducing the parity check end to end**

1. Build the ramp (ffmpeg, libx265 lossless 10-bit, PQ tags).
2. `mpv --vo=gpu-next --pause` on it, screenshot, and keep the numbers as
   the reference (the table in `shader_port.py`).
3. `rex-wallpaper-runtime --file <ramp>` as the wallpaper, screenshot the
   desktop, then `python parity_check.py <capture>`.

The measured result on the reference machine (RX 6700 XT, 4K desktop): the
engine's on-screen wallpaper matches the mpv-parity model to **±1 code**
(median 0.30 over the 24 bands).

## The older set (2026-10-10)

- `shader_port.py` also keeps the SDR pass-through check.
- `make_bands.py` — the 8-band ramp (9.1/40/98/204/397/594/806/1004 nits) via
  `zscale → npl=10000`. NOTE: the ffmpeg default `npl=100` silently encodes
  100× too dim — always pass `npl=10000` explicitly.

Companions that stayed in temp (one-off, superseded, or huge binaries):
band PNGs, `.rgb`/`.raw` frames, `hdr_bands.mp4`, prediction grids.
