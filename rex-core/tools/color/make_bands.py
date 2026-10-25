import struct, zlib, sys

def linear_to_srgb(c):
    if c <= 0.0031308:
        return c * 12.92
    return 1.055 * (max(c, 0.0) ** (1 / 2.4)) - 0.055

def write_png(path, w, h, rows):
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(rows, 6))
    png += chunk(b"IEND", b"")
    open(path, "wb").write(png)

NITS_HDR = [10, 40, 100, 203, 400, 600, 800, 1000]
SDR_CODES = [0, 32, 64, 96, 128, 160, 192, 224]

# The mpv-parity ramp: 24 PQ bands (0.01 .. 4000 cd/m2), left to right, each
# band 1/24 of the width.
#
# The band values come from shader_port.MPV_BANDS - the single source of
# truth. That list is the one mpv 0.41.0's own measurements (MPV_MEASURED_
# 16BIT) were captured on, so the fixture, the parity model and the mpv
# reference can never drift apart.
#
# They DID drift: this file used to carry its own copy of the list, which
# unit 22 (e18de18) changed to [.. 200, 300, 500, 800, 1200, 2000, 3000 ..]
# while shader_port.py kept [.. 203, 300, 400, 600, 800, 1000, 2000 ..]. The
# parity check then compared the clip against the wrong band values and
# reported a +7..+17 code "regression" on the bright bands that the engine
# never had - the clip really did contain 500/800/1200/2000/3000 nits.
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from shader_port import MPV_BANDS as PQ_BANDS_NITS  # noqa: E402


def write_png16(path, w, h, rows):
    """16-bit RGB PNG (the old 8-bit writer is kept for the legacy set)."""
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 16, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(rows, 6))
    png += chunk(b"IEND", b"")
    open(path, "wb").write(png)


def build_pq_ramp(path, w=1920, h=1080):
    """24 vertical bands as RAW 10-bit YUV420 (PQ-encoded luma, neutral chroma).

    The PQ code is computed here (the same EOTF the engine and mpv use), so
    the clip needs no colour filter: ffmpeg only wraps the raw frame and tags
    it. Two reasons this is done by hand:

      * zscale is avoided on purpose - some ffmpeg builds cannot find a path
        between the PNG's sRGB and the PQ/BT.2020 target;
      * the raw frame is written in the PIXEL FORMAT THE ENCODER WANTS
        (yuv420p10le), so no colour conversion happens at all. A 16-bit
        intermediate would go through swscale, and this ffmpeg build's
        YUV->RGB path is broken (it turns a neutral grey frame green).

    FFmpeg stores 10-bit samples in the LOW bits of a 16-bit little-endian
    word (verified: a gray10le value of 1023 decodes to 255 in 8-bit).

        ffmpeg -f rawvideo -pix_fmt yuv420p10le -s 1920x1080 -i ramp.yuv \\
          -c:v libx265 -preset ultrafast -crf 0 -x265-params "lossless=1:\
colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc" \\
          -pix_fmt yuv420p10le -color_primaries bt2020 -color_trc smpte2084 \\
          -colorspace bt2020nc ramp_pq.mp4
    """
    m1, m2 = 0.1593017578125, 78.84375
    c1, c2, c3 = 0.8359375, 18.8515625, 18.6875

    def pq(nits):
        y = max(0.0, nits / 10000.0) ** m1
        return ((c1 + c2 * y) / (1.0 + c3 * y)) ** m2

    n = len(PQ_BANDS_NITS)
    span = w // n
    codes = [int(round(pq(v) * 1023.0)) for v in PQ_BANDS_NITS]
    y_plane = bytearray()
    for _ in range(h):
        for b in range(n):
            y_plane += struct.pack("<H", codes[b]) * span
    neutral = struct.pack("<H", 512) * (w // 2 * (h // 2))
    with open(path, "wb") as f:
        f.write(bytes(y_plane))
        f.write(neutral)
        f.write(neutral)
    return n, span, codes


W, H = 1920, 1080
TOP = 900          # bands occupy y in [900, 1080) -> bottom 180 rows
BAND_H = (H - TOP) // 8

def build(path, band_rgb):
    rows = []
    for y in range(H):
        row = bytearray([0])
        if y >= TOP:
            idx = min((y - TOP) // BAND_H, 7)
            v = band_rgb[idx]
            row += bytes(v) * W
        else:
            row += bytes([0, 0, 0]) * W
        rows.append(bytes(row))
    write_png(path, W, H, b"".join(rows))

def centers():
    return [TOP + k * BAND_H + BAND_H // 2 for k in range(8)]

if __name__ == "__main__":
    import os
    base = os.environ.get("REX_BANDS_DIR",
                          os.path.join(os.environ.get("TEMP", "."), "opencode", "rex-verify"))
    os.makedirs(base, exist_ok=True)
    sdr = [(v, v, v) for v in SDR_CODES]
    hdr = []
    for n in NITS_HDR:
        s = linear_to_srgb(n / 10000.0)
        v = max(0, min(255, int(round(s * 255))))
        hdr.append((v, v, v))
    build(base + r"\bands_sdr.png", sdr)
    build(base + r"\bands_hdr.png", hdr)
    print("BAND_H =", BAND_H, "TOP =", TOP)
    print("band center rows:", centers())
    print("physical sample y (2x):", [c * 2 for c in centers()])
    print("SDR png codes:", SDR_CODES)
    print("HDR png codes:", [v for v, _, _ in hdr])

    # The mpv-parity ramp (24 PQ bands, left to right).
    n, span, codes = build_pq_ramp(base + r"\ramp_pq.yuv")
    print("PQ ramp: %d bands, %d px each ->" % (n, span), base + r"\ramp_pq.yuv")
    print("PQ codes (10-bit):", codes)
    print("encode with:")
    # The range has to be set on the INPUT side: ffmpeg only propagates it
    # into the encoder (and the clip's VUI) when the rawvideo frames carry
    # it. A limited-range clip makes the decoder expand the 10-bit PQ codes,
    # and the ramp then peaks at 8274 nits instead of 4000 - the whole
    # parity comparison is measured against a shifted ramp.
    print('  ffmpeg -stream_loop 24 -color_range pc '
          '-f rawvideo -pix_fmt yuv420p10le -s 1920x1080 '
          '-i "%s\\ramp_pq.yuv" -c:v libx265 -preset ultrafast -crf 0 '
          '-x265-params "lossless=1:colorprim=bt2020:transfer=smpte2084:'
          'colormatrix=bt2020nc" -pix_fmt yuv420p10le '
          '-color_primaries bt2020 -color_trc smpte2084 '
          '-colorspace bt2020nc -color_range pc "%s\\ramp_pq.mp4"' % (base, base))
