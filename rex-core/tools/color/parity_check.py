"""Measures a captured wallpaper and compares it with the mpv-parity model.

This is the reproducible half of the "file == wallpaper" check: point it at a
screenshot of the engine's wallpaper (or of mpv's window) and it prints the
measured code per band next to the model, then exits non-zero if they differ
by more than the tolerance.

    python parity_check.py capture.png [row_y] [--tol 1.5]

The capture must show the 24-band PQ ramp produced by make_bands.py (the
bands run left to right, each 1/24 of the width). Without a row argument the
tool scans the image and picks the row that looks most like the ramp.

The model is the engine's own curve (shader_port.py, a mirror of
HdrToneMapping.hpp + the shader in D3D12Renderer.cpp), which is pinned
against mpv 0.41.0's own measurements by
tests/wallpaper/test_color_pipeline.cpp.
"""

import sys
import zlib

from shader_port import (MPV_BANDS, MPV_MEASURED_16BIT, PqEncode, shader_hdr_to_sdr,
                          tone_curve_for_bands)


# --- a dependency-free PNG reader (8/16-bit, gray/RGB/RGBA, non-interlaced) --
def read_png(path):
    data = open(path, 'rb').read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('not a PNG')
    pos, idat, width, height, depth, color = 8, [], 0, 0, 0, 0
    while pos < len(data):
        length = int.from_bytes(data[pos:pos + 4], 'big')
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if kind == b'IHDR':
            width = int.from_bytes(body[0:4], 'big')
            height = int.from_bytes(body[4:8], 'big')
            depth, color = body[8], body[9]
            if body[12] != 0:
                raise ValueError('interlaced PNG not supported')
        elif kind == b'IDAT':
            idat.append(body)
        elif kind == b'IEND':
            break
        pos += 12 + length
    channels = {0: 1, 2: 3, 4: 2, 6: 4}.get(color)
    if channels is None:
        raise ValueError('unsupported PNG colour type %d' % color)
    if depth not in (8, 16):
        raise ValueError('unsupported PNG bit depth %d' % depth)
    bpp = channels * (depth // 8)
    raw = zlib.decompress(b''.join(idat))
    stride = width * bpp
    out = bytearray(stride * height)
    prev = bytearray(stride)
    pos = 0
    for y in range(height):
        filt = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            v = line[x]
            if filt == 1:
                v = (v + a) & 255
            elif filt == 2:
                v = (v + b) & 255
            elif filt == 3:
                v = (v + ((a + b) >> 1)) & 255
            elif filt == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                v = (v + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
            line[x] = v
        out[y * stride:(y + 1) * stride] = line
        prev = line
    if depth == 16:
        samples = [out[i] * 256 + out[i + 1] for i in range(0, len(out), 2)]
        scale = 65535.0
    else:
        samples = list(out)
        scale = 255.0
    return width, height, channels, samples, scale


def band_median(img, y, band, count):
    width, height, channels, samples, scale = img
    span = width // count
    x0 = band * span + span // 3
    x1 = (band + 1) * span - span // 3
    values = []
    for x in range(x0, x1):
        values.append(samples[(y * width + x) * channels])
    values.sort()
    return values[len(values) // 2] / scale * 255.0


def ramp_score(img, y, count):
    """How much a row looks like the ramp (monotone, spread over the range)."""
    values = [band_median(img, y, b, count) for b in range(count)]
    rising = sum(1 for a, b in zip(values, values[1:]) if b >= a - 1)
    spread = max(values) - min(values)
    return rising + spread / 16.0


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    path = argv[1]
    row = None
    tol = 3.5
    for arg in argv[2:]:
        if arg == '--tol' and argv.index(arg) + 1 < len(argv):
            tol = float(argv[argv.index(arg) + 1])
        elif arg.isdigit():
            row = int(arg)
    img = read_png(path)
    count = len(MPV_BANDS)
    if row is None:
        best, row = -1.0, img[1] // 2
        for y in range(0, img[1], max(1, img[1] // 120)):
            score = ramp_score(img, y, count)
            if score > best:
                best, row = score, y
        print('picked row y=%d (ramp score %.1f)' % (row, best))
    curve = tone_curve_for_bands(MPV_BANDS)
    print(' nits   measured   model(G22)   mpv(sRGB)')
    worst = 0.0
    deltas = []
    for index, (band, mpv16) in enumerate(zip(MPV_BANDS, MPV_MEASURED_16BIT)):
        measured = band_median(img, row, index, count)
        model = shader_hdr_to_sdr(band, curve)[0] * 255.0
        delta = abs(measured - model)
        worst = max(worst, delta)
        deltas.append(delta)
        print('%7.2f  %8.1f  %10.2f  %9.1f  %+7.2f' %
              (band, measured, model, mpv16 / 257.0, measured - model))
    deltas.sort()
    median = deltas[len(deltas) // 2]
    print('\nmax |measured - model| = %.2f codes, median %.2f (tolerance %.1f)' %
          (worst, median, tol))
    print('(the desktop capture path costs a couple of codes: the wallpaper is')
    print(' upscaled to the 4K desktop, the back buffer is 8-bit and the DWM')
    print(' applies the display profile on the way to the screen.)')
    if worst > tol:
        print('FAIL: the capture does not match the mpv-parity model')
        return 1
    print('PASS: the capture matches the mpv-parity model')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
