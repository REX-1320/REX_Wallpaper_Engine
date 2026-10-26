#!/usr/bin/env python3
"""Generate Paiint's icons from the master logo.

The mark is the supplied gold/black four-petal artwork (transparent PNG).
This tool crops it to its content, fits it into a square canvas and writes:

  app/assets/paiint-app.ico   multi-size app/exe icon (electron-builder)
  app/assets/paiint-tray.ico  tray icon
  app/src/assets/paiint-mark.png  512 px mark for the About page

The ICO frames are PNG-compressed (the container is written by hand because
Pillow's default ICO writer emits BMP frames, and packaging/msix/build-msix.ps1
decodes the largest frame as PNG).

Run from the repo root:  python app/tools/make-icons.py
"""

from io import BytesIO
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]          # app/
MASTER = ROOT / "assets" / "paiint-mark.png"
APP_ICON = ROOT / "assets" / "paiint-app.ico"
TRAY_ICON = ROOT / "assets" / "paiint-tray.ico"
UI_MARK = ROOT / "src" / "assets" / "paiint-mark.png"

ICON_SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)
TRAY_SIZES = (16, 20, 24, 32, 40, 48, 64, 256)


def load_mark() -> Image.Image:
    mark = Image.open(MASTER).convert("RGBA")
    bbox = mark.getbbox()
    if bbox is None:
        raise SystemExit(f"{MASTER} is fully transparent")
    return mark.crop(bbox)


def frame(mark: Image.Image, size: int, fill: float) -> Image.Image:
    """The mark centred on a transparent square, `fill` of the side used."""
    target = max(1, round(size * fill))
    scale = min(target / mark.width, target / mark.height)
    scaled = mark.resize(
        (max(1, round(mark.width * scale)), max(1, round(mark.height * scale))),
        Image.LANCZOS,
    )
    canvas = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    canvas.alpha_composite(
        scaled, ((size - scaled.width) // 2, (size - scaled.height) // 2)
    )
    return canvas


def write_ico(path: Path, mark: Image.Image, sizes, fill: float) -> None:
    frames = [(s, frame(mark, s, fill)) for s in sizes]
    payload = []
    for _, img in frames:
        from io import BytesIO

        buf = BytesIO()
        img.save(buf, format="PNG", optimize=True)
        payload.append(buf.getvalue())

    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"\x00\x00")                        # reserved
        f.write(b"\x01\x00")                        # type = icon
        f.write(len(frames).to_bytes(2, "little"))
        offset = 6 + 16 * len(frames)
        for (size, _), data in zip(frames, payload):
            dim = 0 if size >= 256 else size
            f.write(bytes((dim, dim, 0, 0)))
            f.write((1).to_bytes(2, "little"))      # planes
            f.write((32).to_bytes(2, "little"))     # bpp
            f.write(len(data).to_bytes(4, "little"))
            f.write(offset.to_bytes(4, "little"))
            offset += len(data)
        for data in payload:
            f.write(data)
    print(f"{path.relative_to(ROOT.parent)}: {len(frames)} sizes, {path.stat().st_size} bytes")


def main() -> None:
    mark = load_mark()
    print(f"master: {mark.width}x{mark.height}")

    write_ico(APP_ICON, mark, ICON_SIZES, fill=0.94)
    write_ico(TRAY_ICON, mark, TRAY_SIZES, fill=0.98)

    UI_MARK.parent.mkdir(parents=True, exist_ok=True)
    ui = frame(mark, 512, 0.98)
    ui.save(UI_MARK, format="PNG", optimize=True)
    print(f"{UI_MARK.relative_to(ROOT.parent)}: 512 px, {UI_MARK.stat().st_size} bytes")


if __name__ == "__main__":
    main()
