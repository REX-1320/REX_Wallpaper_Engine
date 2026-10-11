# tools/color — color-accuracy verification scripts

Rescued 2026-10-10 from `%TEMP%\opencode\rex-verify\` (wipe-prone).

- `shader_port.py` — exact Python port of the D3D12 pixel shader used to
  predict ITU-R BT.2408 targets (203 nits → ~232/255) and prove the A11–A13
  fixes numerically before the on-GPU band test.
- `make_bands.py` — generates the known-luminance HDR band test video
  (`hdr_bands.mp4`: 9.1/40/98/204/397/594/806/1004 nits) via
  `zscale … npl=10000`. NOTE: the ffmpeg default `npl=100` silently encodes
  100× too dim — always pass `npl=10000` explicitly.

Companions that stayed in temp (one-off, superseded, or huge binaries):
band PNGs, `.rgb`/`.raw` frames, `hdr_bands.mp4`, prediction grids.
