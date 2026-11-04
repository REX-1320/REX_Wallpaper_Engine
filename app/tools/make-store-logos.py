#!/usr/bin/env python3
"""Generate the Microsoft Store listing logos from the master mark.

Master: app/assets/paiint-mark.png (the rasterised paiint-mark.svg, transparent).

Outputs into the directory given with --out:

  StoreLogo-300x300.png    1:1 listing logo
  StoreLogo-300x150.png    2:1 listing logo
  StoreLogo-1080x1080.png  1:1 large logo

Layout is deterministic: the mark is cropped to content, fit into a square
box of FILL x the constraining side, centred, on a fully transparent canvas.

Run from the repo root:  python app/tools/make-store-logos.py --out <dir>
"""

import argparse
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]          # app/
MASTER = ROOT / "assets" / "paiint-mark.png"
FILL = 0.90
# (w, h, fill-of-constraining-side, background) — None bg = transparent.
# The three listing logos are transparent; the optional Partner Center art
# slots (16:9 poster, super-hero) sit on a dark backdrop because the Store
# tiles them on dark grey and the mark's black petals need a light-enough
# ground to read.
TARGETS = (
    (300, 300, FILL, None),
    (300, 150, FILL, None),
    (1080, 1080, FILL, None),
    (1440, 2160, 0.80, (31, 31, 35, 255)),   # PosterArt (Xbox/hero slot, portrait)
    (720, 1080, 0.80, (31, 31, 35, 255)),    # PosterArt small
    (1920, 1080, 0.70, (31, 31, 35, 255)),   # 16:9 Super hero art
)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True, help="directory for the generated logos")
    args = ap.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    mark = Image.open(MASTER).convert("RGBA")
    bbox = mark.getbbox()
    if bbox is None:
        raise SystemExit(f"{MASTER} is fully transparent")
    content = mark.crop(bbox)
    print(f"master content: {content.width}x{content.height}")

    for w, h, fill, bg in TARGETS:
        side = round(min(w, h) * fill)
        scale = min(side / content.width, side / content.height)
        cw = max(1, round(content.width * scale))
        ch = max(1, round(content.height * scale))
        scaled = content.resize((cw, ch), Image.LANCZOS)

        dst = Image.new("RGBA", (w, h), bg or (0, 0, 0, 0))
        dst.alpha_composite(scaled, ((w - cw) // 2, (h - ch) // 2))
        name = f"StoreLogo-{w}x{h}.png" if bg is None else (
            f"PosterArt-{w}x{h}.png" if h > w else f"HeroArt-{w}x{h}.png"
        )
        path = out / name
        dst.save(path, format="PNG", optimize=True)

        alpha = dst.getchannel("A")
        alpha_px = alpha.load()
        bb = alpha.getbbox()
        corners = [alpha_px[p] for p in ((0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1))]
        grid = len(range(0, w, 3)) * len(range(0, h, 3))
        samples = sum(
            1 for gx in range(0, w, 3) for gy in range(0, h, 3) if alpha_px[gx, gy] >= 32
        )
        print(
            f"{path.name}: {w}x{h}, content {cw}x{ch} at (({w - cw})//2={(w - cw) // 2},"
            f" {(h - ch) // 2}), alpha bbox {bb}, corners {corners},"
            f" {samples}/{grid} grid samples alpha>=32"
        )


if __name__ == "__main__":
    main()
