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
    base = r"C:\Users\Moinu\AppData\Local\Temp\opencode\rex-verify"
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
