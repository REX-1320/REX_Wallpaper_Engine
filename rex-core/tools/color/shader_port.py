"""Exact port of g_pixelShaderSource (D3D12Renderer.cpp:47-255) for the
HDR P010 -> SDR output path (hdrOutput=0, transfer=PQ, format=P010).
"""
import math

def saturate(v): return max(0.0, min(1.0, v))

def PqToLinear(e):
    m1, m2, c1, c2, c3 = 0.1593017578125, 78.84375, 0.8359375, 18.8515625, 18.6875
    p = max(e, 0.0) ** (1.0 / m2)
    num = max(p - c1, 0.0)
    den = c2 - c3 * p
    return (num / max(den, 1e-9)) ** (1.0 / m1) * 10000.0

def AcesFit(x):
    return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14))

def LinearToSrgb(c):
    if c <= 0.0031308:
        return c * 12.92
    return 1.055 * (max(c, 0.0) ** (1.0 / 2.4)) - 0.055

def LinearToPq(l):
    m1, m2, c1, c2, c3 = 0.1593017578125, 78.84375, 0.8359375, 18.8515625, 18.6875
    p = max(l / 10000.0, 0.0) ** m1
    return ((c1 + c2 * p) / (1.0 + c3 * p)) ** m2

BT2020_TO_BT709 = [[1.6605, -0.1246, -0.0182],
                    [-0.5876, 1.1323, -0.1006],
                    [-0.0728, -0.0077, 1.1187]]

def row_mul(v, M):
    # HLSL mul(float3, float3x3) == row-vector * matrix == dot(row, column_j)
    return [sum(v[k] * M[k][j] for k in range(3)) for j in range(3)]

def shader_sdr_output(nits, mastering_max=1000.0, norm_mode="mastermax",
                      paper_white=203.0, preclamp=True, matrix="row",
                      yuv_coeffs="709"):
    """HDR P010 source -> SDR (sRGB) output. nits = absolute scene luminance.
    Returns 0..1 signal written to the 8-bit SDR back buffer.
    """
    norm = mastering_max if norm_mode == "mastermax" else paper_white

    # --- P010 branch: emulate the encoder side ---
    # content is encoded in PQ; shader does: PqToLinear(e)/norm
    e = LinearToPq(nits)
    rgb = [PqToLinear(e)] * 3
    rgb = [v / norm for v in rgb]                       # line 180

    if matrix == "row":                                 # line 182
        rgb = [max(v, 0.0) for v in row_mul(rgb, BT2020_TO_BT709)]
    elif matrix == "col":                               # the buggy orientation
        rgb = [max(v, 0.0) for v in row_mul(rgb, [list(c) for c in zip(*BT2020_TO_BT709)])]

    # --- line 214-218: mode.w < 0.5 (SDR output) ---
    if preclamp:
        rgb = [saturate(v * (1.0 * 1.0)) for v in rgb]
    else:
        rgb = [max(v, 0.0) for v in rgb]

    # --- line 240-248: mode.w > 0.5 skipped (SDR) ---
    # --- line 250-252: ACES applied (transfer==PQ) ---
    rgb = [AcesFit(v) for v in rgb]
    # --- line 253 ---
    return LinearToSrgb(saturate(rgb[0]))


def SrgbToLinear(c):
    def f(x):
        x = max(x, 0.0)
        return x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4
    return [f(c[0]), f(c[1]), f(c[2])]

def shader_sdr_video_output(signal_8bit, fixed=False):
    """SDR NV12 video -> SDR output. signal_8bit = gamma-encoded R'G'B' (0..255).
    Path: lines 193-219 (YUV->RGB, NO linearization), 221-225, 263 (no ACES), 265.
    """
    rgb = signal_8bit / 255.0
    if fixed:
        rgb = SrgbToLinear([saturate(rgb), saturate(rgb), saturate(rgb)])[0]
    rgb = saturate(rgb * 1.0)            # line 222
    return LinearToSrgb(rgb)             # line 265 (ACES skipped: mode.y=0, x=0)


if __name__ == "__main__":
    nits_list = [9.1, 40.3, 97.7, 203.7, 397.2, 594.0, 805.8, 1004.2]

    print("=" * 74)
    print("A) HDR10 content (P010) -> SDR output, BT.2020 source, neutral gray")
    print("=" * 74)
    print(f"{'nits':>8} | {'BEFORE':>24} | {'AFTER (fixed)':>17}")
    print(f"{'':>8} | {'norm=1000, pre-clamp':>17} | {'norm=203, no clamp':>17}")
    print("-" * 74)
    for n in nits_list:
        cur = shader_sdr_output(n, norm_mode="mastermax", preclamp=True)
        b = shader_sdr_output(n, norm_mode="paper", preclamp=False)
        print(f"{n:8.1f} | {cur:8.4f} -> {round(cur*255):3d} | {b:8.4f} -> {round(b*255):3d}")
    print()
    print("ITU-R BT.2408 target: 203 nits paper white ~ 0.90 of signal (~229)")

    print()
    print("=" * 74)
    print("B) SDR NV12 video: source code value -> rendered")
    print("=" * 74)
    print(f"{'src':>6} | {'BEFORE':>16} | {'AFTER (fixed)':>16} | {'expected':>9}")
    print("-" * 74)
    for v in (0, 32, 64, 96, 128, 160, 192, 224):
        old = shader_sdr_video_output(v, fixed=False)
        new = shader_sdr_video_output(v, fixed=True)
        print(f"{v:6d} | {old*255:8.2f} -> {round(old*255):3d} | {new*255:8.2f} -> "
              f"{round(new*255):3d} | {v:9d}")
    print()
    print("identity = pixel-perfect pass-through (what a correct renderer gives)")
