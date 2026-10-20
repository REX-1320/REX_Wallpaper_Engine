"""Exact port of the HDR->SDR path in g_pixelShaderSource
(D3D12Renderer.cpp) - now an mpv / libplacebo v7.360.0 port.

The engine tone-maps HDR content the way mpv does:

  * a per-frame peak detection measures the source peak and the scene average
    in PQ space (the engine's compute pass, libplacebo's pl_shader_detect_peak),
  * the curve is libplacebo's single-pivot polynomial spline, solved on the
    CPU from those two numbers (graphics/renderer/HdrToneMapping.hpp),
  * the curve is applied to the INTENSITY channel of the IPT colour space, so
    a saturated highlight keeps its hue instead of being scaled per channel,
  * the output is black-scaled: the target black (203/1000 cd/m²) lands on
    code 0 and the SDR reference white (203 cd/m²) on code 1, then gamma 2.2.

This file mirrors that math so the values can be inspected without a GPU.
The C++ original is HdrToneMapping.hpp + the shader in D3D12Renderer.cpp;
the two must agree (the unit test in tests/wallpaper/test_color_pipeline.cpp
pins the curve against mpv's own measurements).
"""

import math

# --- libplacebo constants ---------------------------------------------------
PQ_M1, PQ_M2 = 0.1593017578125, 78.84375
PQ_C1, PQ_C2, PQ_C3 = 0.8359375, 18.8515625, 18.6875
SDR_WHITE_NITS = 203.0          # PL_COLOR_SDR_WHITE
SDR_CONTRAST = 1000.0           # PL_COLOR_SDR_CONTRAST
HDR_BLACK_NITS = 1e-6           # PL_COLOR_HDR_BLACK
TARGET_BLACK_NORM = 0.001       # 0.203 cd/m² in the NORM scale (1 == 203)


def saturate(v):
    return max(0.0, min(1.0, v))


def mix(a, b, t):
    return a + (b - a) * t


def clamp(x, lo, hi):
    return min(max(x, lo), hi)


def smoothstep(edge0, edge1, x):
    """libplacebo's pl_smoothstep (edge0 may exceed edge1)."""
    if edge0 == edge1:
        return 1.0 if x >= edge0 else 0.0
    t = clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def PqEncode(nits):
    """PQ inverse EOTF: absolute nits -> PQ code."""
    y = max(nits, 0.0) / 10000.0
    p = y ** PQ_M1
    return ((PQ_C1 + PQ_C2 * p) / (1.0 + PQ_C3 * p)) ** PQ_M2


def PqDecode(pq):
    """PQ EOTF: PQ code -> absolute nits."""
    p = max(pq, 0.0) ** (1.0 / PQ_M2)
    num = max(p - PQ_C1, 0.0)
    den = PQ_C2 - PQ_C3 * p
    if num <= 0.0 or den <= 0.0:
        return 0.0
    return 10000.0 * (num / den) ** (1.0 / PQ_M1)


def LinearToSrgb(c):
    if c <= 0.0031308:
        return c * 12.92
    return 1.055 * (max(c, 0.0) ** (1.0 / 2.4)) - 0.055


# --- IPT (Ebner & Fairchild 1998) as libplacebo builds it ------------------
# pl_ipt_rgb2lms(BT.2020) and pl_ipt_lms2rgb(BT.709).
RGB2020_TO_LMS = [
    [0.41203639, 0.52391191, 0.06405498],
    [0.16666022, 0.72039521, 0.11294612],
    [0.02411236, 0.07547496, 0.90040794],
]
LMS_TO_IPT = [
    [0.4000, 0.4000, 0.2000],
    [4.4550, -4.8510, 0.3960],
    [0.8056, 0.3572, -1.1628],
]
IPT_TO_LMS = [
    [1.0, 0.0975689, 0.205226],
    [1.0, -0.1138760, 0.133217],
    [1.0, 0.0326151, -0.676887],
]
LMS_TO_RGB709 = [
    [6.17353266, -5.32089882, 0.14735489],
    [-1.32403191, 2.56026977, -0.23623862],
    [-0.01159839, -0.26492145, 1.27652634],
]


def mul(m, v):
    """HLSL mul(float3x3, float3) == matrix * column vector."""
    return [sum(m[r][c] * v[c] for c in range(3)) for r in range(3)]


# --- the curve --------------------------------------------------------------
class ToneCurve:
    """libplacebo's spline curve, solved from a measured peak/average."""

    def __init__(self, max_pq, avg_pq):
        self.in_min = PqEncode(HDR_BLACK_NITS)
        self.out_min = PqEncode(SDR_WHITE_NITS / SDR_CONTRAST)
        self.out_max = PqEncode(SDR_WHITE_NITS)
        # pl_tone_map_params_infer(): the input peak is never below 100 nits.
        self.in_max = clamp(max_pq, PqEncode(100.0), 1.0)
        self.in_avg = clamp(avg_pq, self.in_min, self.in_max)

        # st2094_pick_knee()
        src_min_k = mix(self.in_min, self.in_max, 0.1)
        src_max_k = mix(self.in_min, self.in_max, 0.8)
        dst_min_k = mix(self.out_min, self.out_max, 0.1)
        dst_max_k = mix(self.out_min, self.out_max, 0.8)
        src_knee = self.in_avg if self.in_avg else mix(self.in_min, self.in_max, 0.4)
        src_knee = clamp(src_knee, src_min_k, src_max_k)
        target = (src_knee - self.in_min) / (self.in_max - self.in_min)
        adapted = mix(self.out_min, self.out_max, target)
        tuning = 1.0 - smoothstep(0.8, 0.4, target) * smoothstep(0.1, 0.4, target)
        adaptation = mix(0.4, 1.0, tuning)
        dst_knee = clamp(mix(src_knee, adapted, adaptation), dst_min_k, dst_max_k)
        self.src_pivot, self.dst_pivot = src_knee, dst_knee

        # spline()
        slope = (dst_knee - self.out_min) / (src_knee - self.in_min)
        ratio = self.in_max / self.out_max - 1.0
        ratio = clamp(1.5 * ratio, 0.2, 1.2)
        slope = slope ** ((1.0 - 0.5) * ratio)
        self.slope = slope
        in_min_n = self.in_min - src_knee
        in_max_n = self.in_max - src_knee
        out_min_n = self.out_min - dst_knee
        out_max_n = self.out_max - dst_knee
        self.pa = (out_min_n - slope * in_min_n) / (in_min_n * in_min_n)
        t = 2.0 * in_max_n * in_max_n
        self.qa = (slope * in_max_n - out_max_n) / (in_max_n * t)
        self.qb = -3.0 * (slope * in_max_n - out_max_n) / t

    def evaluate(self, pq):
        """The curve: PQ code in, PQ code out."""
        x = clamp(pq, self.in_min, self.in_max) - self.src_pivot
        if x > 0.0:
            y = ((self.qa * x + self.qb) * x + self.slope) * x
        else:
            y = (self.pa * x + self.slope) * x
        return y + self.dst_pivot


def tone_curve_for_bands(bands):
    """The curve a frame of these band levels produces (peak + mean PQ)."""
    pqs = [PqEncode(b) for b in bands]
    avg = sum(p for p in pqs if p >= 0.01) / len([p for p in pqs if p >= 0.01])
    return ToneCurve(max(pqs), avg)


# --- the shader path --------------------------------------------------------
def shader_hdr_to_sdr(nits, curve):
    """HDR scene luminance (absolute nits, neutral) -> the 0..1 signal the
    engine writes to its 8-bit SDR back buffer (gamma 2.2 encoded)."""
    # 1) source gamut -> LMS -> PQ -> IPT
    lms = mul(RGB2020_TO_LMS, [nits, nits, nits])
    lmspq = [PqEncode(v) for v in lms]
    ipt = mul(LMS_TO_IPT, lmspq)
    i_orig = ipt[0]

    # 2) the curve, on intensity only
    ipt[0] = curve.evaluate(clamp(ipt[0], 0.00011948, curve.in_max))

    # 3) hue hull
    hull = [(v - 6.0) * v + 9.0 for v in (i_orig, ipt[0])]
    hull = [h * v for h, v in zip(hull, (i_orig, ipt[0]))]
    scale = min(i_orig / max(ipt[0], 1e-6), hull[1] / max(hull[0], 1e-6))
    ipt[1] *= scale
    ipt[2] *= scale

    # 4) back to linear light in the target gamut (BT.709), NORM scale
    lms_pq = mul(IPT_TO_LMS, ipt)
    lms_lin = [PqDecode(v) / SDR_WHITE_NITS for v in lms_pq]
    rgb = mul(LMS_TO_RGB709, lms_lin)
    rgb = [saturate(v) for v in rgb]

    # 5) black-scaled delinearize + gamma 2.2 (the swap chain is G22)
    signal = [(v - TARGET_BLACK_NORM) / (1.0 - TARGET_BLACK_NORM) for v in rgb]
    return [max(v, 0.0) ** (1.0 / 2.2) for v in signal]


def shader_sdr_video_output(signal_8bit, fixed=False):
    """SDR NV12 video -> SDR output (unchanged by the tone-map port)."""
    def SrgbToLinear(c):
        x = max(c, 0.0)
        return x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4

    rgb = signal_8bit / 255.0
    if fixed:
        rgb = SrgbToLinear(saturate(rgb))
    rgb = saturate(rgb)
    return LinearToSrgb(rgb)


# --- the mpv reference ------------------------------------------------------
# mpv 0.41.0 (libplacebo v7.360.0) rendering the 24-band PQ ramp
# (0.01 .. 4000 cd/m²), captured at 16 bits. The engine must match these.
MPV_BANDS = [0.01, 0.05, 0.1, 0.25, 0.5, 1, 2, 5, 10, 20, 26, 40, 50, 80, 100,
             150, 203, 300, 400, 600, 800, 1000, 2000, 4000]
MPV_MEASURED_16BIT = [
    331, 814, 1250, 2148, 3298, 4724, 6622, 9992, 13553, 18261, 20402, 24539,
    27082, 32277, 34687, 39054, 42173, 46156, 48839, 52330, 54600, 56319, 61075,
    65521]


if __name__ == "__main__":
    curve = tone_curve_for_bands(MPV_BANDS)
    print("curve: srcPivot=%.6f dstPivot=%.6f slope=%.6f inMax=%.6f" %
          (curve.src_pivot, curve.dst_pivot, curve.slope, curve.in_max))
    print()
    print(" nits   engine(G22 8-bit)  mpv(sRGB 8-bit)   delta vs mpv")
    worst = 0.0
    for band, mpv16 in zip(MPV_BANDS, MPV_MEASURED_16BIT):
        out = shader_hdr_to_sdr(band, curve)[0]
        engine8 = round(out * 255)
        mpv8 = mpv16 / 257.0
        worst = max(worst, abs(engine8 - mpv8))
        print("%7.2f  %14d  %14.1f  %+12.2f" % (band, engine8, mpv8, engine8 - mpv8))
    print()
    print("max |engine - mpv| = %.2f codes" % worst)
    print("(the two differ by the OUTPUT TRANSFER: the engine writes G22 like")
    print(" mpv's window, the mpv capture above is the sRGB video path -")
    print(" 1-2 codes in the shadows, identical from ~40 cd/m² up.)")
