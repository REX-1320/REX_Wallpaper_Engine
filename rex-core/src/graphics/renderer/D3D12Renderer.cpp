
#include "D3D12Renderer.hpp"
#include "graphics/renderer/HdrSyncPolicy.hpp"
#include "graphics/renderer/OutputDither.hpp"
#include "graphics/renderer/SdrWhiteLevel.hpp"
#include "wallpaper/decoder/WICTextureLoader.hpp"
#include <shared/Logger.hpp>
#include <d3dcompiler.h>
#include <wincodec.h>
#include <ocidl.h>
#include <oleauto.h>

#include <vector>

// DXGI_STATUS_OCCLUDED was introduced in dxgi1_2 / Windows 8 SDK.
#ifndef DXGI_STATUS_OCCLUDED
#define DXGI_STATUS_OCCLUDED 0x087A0001
#endif

namespace rex::wallpaper {

namespace {
    bool IsDeviceRemovedHr(HRESULT hr) {
        return hr == DXGI_ERROR_DEVICE_REMOVED ||
               hr == DXGI_ERROR_DEVICE_RESET ||
               hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
    }
}

D3D12_SHADER_RESOURCE_VIEW_DESC MakeVideoSlotSrvDescriptor(DXGI_FORMAT fmt) {
    D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
    desc.Format = fmt;
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    // The fetched texel is ALWAYS in canonical R,G,B,A order, whatever the
    // format's byte order is in memory: a B8G8R8A8_UNORM texture sampled with
    // the default mapping yields (R,G,B,A) in the shader (the same reason
    // BGRA swap chains sample correctly). The
    // "FROM_MEMORY_COMPONENT_n" enum therefore indexes the fetched
    // components (0=red, 1=green, 2=blue, 3=alpha), not the raw bytes.
    //
    // The old code treated them as raw bytes and remapped the BGRA pool as
    // R<-byte2, G<-byte1, B<-byte0 — which swaps red and blue for every
    // software-decoded wallpaper (skin tones rendered blue, red indicator
    // lamps rendered blue). The default mapping is correct for every format,
    // so no per-format swizzle is needed.
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Texture2D.MipLevels = 1;
    return desc;
}

// ─── Shader Sources ───────────────────────────────────────────────────────

static const char* g_vertexShaderSource = R"(
struct VSInput {
    float2 pos : POSITION;
    float2 uv : TEXCOORD;
};
struct VSOutput {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD;
};

// Rotation/flip matrix (row-major, root constants b1). Identity by default.
cbuffer TransformCB : register(b1) {
    float4x4 transformMatrix;
};

VSOutput main(VSInput input) {
    VSOutput output;
    output.pos = mul(float4(input.pos, 0.0f, 1.0f), transformMatrix);
    output.uv = input.uv;
    return output;
}
)";

// The pixel shader is split across two literals: MSVC caps a single raw
// string literal at 16 KB and the shader grew past that. The two halves
// are concatenated when the shader is compiled.
static const char* g_pixelShaderSourceA = R"(
Texture2D texY : register(t0);
Texture2D texUV : register(t1);
SamplerState sam : register(s0);

cbuffer ColorConstants : register(b0) {
    float4 mode;    // x = matrix (0=BT601, 1=BT709, 2=BT2020)
                    // y = transfer (0=SDR, 1=PQ, 2=HLG)
                    // z = fullRange, w = HDR output
    float4 adj;     // x = brightness, y = contrast, z = saturation, w = paper white nits
    float4 disp;    // x = display peak nits, y = mastering max, z = mastering min, w = MaxCLL
    float3 extra;   // x = inputFormat (0=NV12, 1=RGBA16, 2=P010)
                    // y = staticRGBA (1 = static RGBA texture, skip YUV decode)
                    // z = SDR diffuse white nits (~100, BT.2408; >1 = valid)
};

cbuffer AudioConstants : register(b2) {
    float4 spec[8];
};

struct VSOutput {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD;
};

static const float3x3 BT709_TO_BT2020 = {
    0.6274, 0.0691, 0.0164,
    0.3293, 0.9195, 0.0880,
    0.0433, 0.0114, 0.8956
};

// ── HDR -> SDR tone mapping, mpv / libplacebo parity ─────────────────────
//
// libplacebo v7.360.0 (the build in mpv 0.41.0) tone-maps the *intensity*
// channel of the IPT colour space, not each RGB channel: the highlight hue
// survives the compression instead of being scaled per channel. The matrices
// below are libplacebo's own:
//   pl_ipt_rgb2lms(BT.2020)  - source gamut -> LMS
//   pl_ipt_lms2ipt           - Ebner & Fairchild IPT (1998)
//   pl_ipt_ipt2lms           - numerically inverted
//   pl_ipt_lms2rgb(BT.709)   - target gamut <- LMS
// (see HdrToneMapping.hpp for the curve itself and the CPU-side reference).
static const float3x3 RGB2020_TO_LMS = {
    0.41203639, 0.52391191, 0.06405498,
    0.16666022, 0.72039521, 0.11294612,
    0.02411236, 0.07547496, 0.90040794
};
static const float3x3 LMS_TO_IPT = {
    0.4000, 0.4000, 0.2000,
    4.4550, -4.8510, 0.3960,
    0.8056, 0.3572, -1.1628
};
static const float3x3 IPT_TO_LMS = {
    1.0, 0.0975689, 0.205226,
    1.0, -0.1138760, 0.133217,
    1.0, 0.0326151, -0.676887
};
static const float3x3 LMS_TO_RGB709 = {
    6.17353266, -5.32089882, 0.14735489,
    -1.32403191, 2.56026977, -0.23623862,
    -0.01159839, -0.26492145, 1.27652634
};

float3 PqToLinear(float3 e) {
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    float3 p = pow(max(e, 0.0), 1.0 / m2);
    float3 num = max(p - c1, 0.0);
    float3 den = c2 - c3 * p;
    return pow(num / max(den, 1e-9), 1.0 / m1) * 10000.0;
}
float3 LinearToPq(float3 l) {
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    float3 p = pow(max(l / 10000.0, 0.0), m1);
    return pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}
// HLG (ARIB STD-B67) inverse OETF, nominal 1000-nit peak, system gamma 1.2.
float3 HlgToLinear(float3 e) {
    const float a = 0.17883277;
    const float b = 0.28466892;
    const float c = 0.55991073;
    float3 l = (e <= 0.5) ? (e * e) / 3.0 : (exp((e - c) / a) + b) / 12.0;
    return pow(l, 1.2) * 1000.0;
}

float3 SrgbToLinear(float3 c) {
    float3 lo = c / 12.92;
    float3 hi = pow(max((c + 0.055) / 1.055, 0.0), 2.4);
    return lerp(hi, lo, step(c, float3(0.04045, 0.04045, 0.04045)));
}
float3 LinearToSrgb(float3 c) {
    float3 lo = c * 12.92;
    float3 hi = 1.055 * pow(max(c, 0.0), 1.0 / 2.4) - 0.055;
    return lerp(hi, lo, step(c, float3(0.0031308, 0.0031308, 0.0031308)));
}

// (The former HDR->SDR path used this Hable filmic fit - the curve VLC 3.x
// ships. It crushed PQ shadows (27 % of a dark HDR frame below code 4 vs
// mpv's 9 %), which is what the "too much black" report was about. The
// libplacebo spline below replaced it; the VLC shape stays in git history
// for the planned "VLC-compatible" preset.)

// HDR -> SDR tone curve, mpv / libplacebo v7.360.0 parity.
//
// The spline is solved on the CPU (HdrToneMapping.hpp) and shipped as five
// floats in the colour constants:
//   disp.x = source pivot (PQ)   disp.y = destination pivot (PQ)
//   disp.z = slope at the pivot  disp.w = toe coefficient (Pa)
//   extra.z = input peak (PQ)
// The shoulder coefficients (Qa, Qb) are derived here from the same inputs,
// exactly as libplacebo's spline() derives them.
float ToneMapSpline(float x) {
    const float inMax = extra.z;
    const float imax  = max(inMax - disp.x, 1e-6);
    const float omax  = 0.58068888 - disp.y;   // PQ(203) - destination pivot
    const float num   = disp.z * imax - omax;
    const float qa    = num / (2.0 * imax * imax * imax);
    const float qb    = -1.5 * num / (imax * imax);
    const float xs    = x - disp.x;
    const float y     = xs > 0.0 ? ((qa * xs + qb) * xs + disp.z) * xs
                                 : (disp.w * xs + disp.z) * xs;
    return saturate(y + disp.y);
}

float SampleSpectrum(float x) {
    float idx = x * 31.0;
    int lo = (int)floor(idx);
    int hi = min(lo + 1, 31);
    float t = idx - (float)lo;
    int slot = lo >> 2;
    int lane = lo & 3;
    int slotH = hi >> 2;
    int laneH = hi & 3;
    float vlo = spec[slot][lane];
    float vhi = spec[slotH][laneH];
    return lerp(vlo, vhi, t);
}
)";

static const char* g_pixelShaderSourceB = R"(
// ── Output dither ─────────────────────────────────────────────────────────
//
// The SDR back buffer is a UNORM target, so the final write quantises the
// encoded signal to a whole number of codes: 1024 levels on the 10-bit
// swapchain (mpv's window format), 256 on the 8-bit fallback. Truncating
// a smooth gradient into discrete steps is the banding the colour TODO
// filed. mpv dithers by default (--dither=fruit, a blue-noise template);
// this does the same job with Interleaved Gradient Noise (Jimenez) - a
// fine, high-frequency pattern that reads like blue noise but needs no
// texture and no extra pass.
//
// The amplitude is ONE CODE of the target depth, added to the ENCODED
// signal (after the transfer function, where the quantisation actually
// happens). TARGET_BITS is compiled per pipeline state from the
// back-buffer format, so the amplitude always matches the target; the
// FP16 HDR output is not quantised and dithers not at all.
float IgnDither(float2 p) {
    return frac(52.9829189f * frac(dot(p, float2(0.06711056f, 0.00583715f)))) - 0.5f;
}
#if TARGET_BITS > 0
#define APPLY_DITHER(signal, pixel) \
    saturate((signal) + IgnDither(pixel) * (1.0 / float((1 << TARGET_BITS) - 1)))
#else
#define APPLY_DITHER(signal, pixel) (signal)
#endif

// ── Upscale (the wallpaper is one fullscreen quad) ────────────────────────
//
// A video smaller than the desktop is magnified in this shader, and the
// sampler's bilinear filter is visibly soft - mpv's gpu-next defaults to
// --scale=lanczos. The LUMA is resampled with a Catmull-Rom (bicubic)
// kernel, the "at minimum a Catmull-Rom pass" the colour TODO asks for;
// the CHROMA keeps the bilinear tap (it is subsampled anyway, and doubling
// the tap count for it would cost more than it shows).
//
// 16 taps (4x4), and only when the source is actually magnified: the
// magnification factor comes from the UV derivatives (source texels per
// output pixel), so no extra root constants are needed - the constant
// budget is exactly 64/64 DWORDs.
//
// SampleLevel (not Sample) inside the taps: the derivative-based mip
// selection is undefined in the loop's control flow, and the pool textures
// have a single mip level anyway.
void CatmullRomWeights(float t, out float w0, out float w1, out float w2,
                        out float w3) {
    const float t2 = t * t;
    const float t3 = t2 * t;
    w0 = -0.5f * t3 + t2 - 0.5f * t;
    w1 = 1.5f * t3 - 2.5f * t2 + 1.0f;
    w2 = -1.5f * t3 + 2.0f * t2 + 0.5f * t;
    w3 = 0.5f * t3 - 0.5f * t2;
}

float SampleLumaCatmullRom(Texture2D tex, SamplerState sam, float2 uv) {
    uint w, h;
    tex.GetDimensions(w, h);
    const float2 src = float2(w, h);
    // Uniform control flow: the derivatives must be taken before any branch.
    const float2 d = float2(ddx(uv.x), ddy(uv.y));
    // scale = source texels per OUTPUT pixel. A magnified source (the video
    // is smaller than the desktop) therefore has scale < 1, and a 1:1 or
    // minified source has scale >= 1.
    //
    // The kernel exists for the UPSCALE path, so the early-out must be the
    // other way round: the inverted test (scale <= 1) meant the kernel never
    // ran on an upscale - the one case it exists for - while a minified
    // source paid for 16 taps it did not need.
    const float2 scale = float2(abs(d.x) * src.x, abs(d.y) * src.y);
    if (scale.x >= 1.0001f && scale.y >= 1.0001f) {
        return tex.SampleLevel(sam, uv, 0.0).r;   // 1:1 or minified
    }
    const float2 pos = uv * src - 0.5;
    const float2 f = frac(pos);
    const float2 base = (floor(pos) + 0.5) / src;   // texel centres
    float wx0, wx1, wx2, wx3;
    float wy0, wy1, wy2, wy3;
    CatmullRomWeights(f.x, wx0, wx1, wx2, wx3);
    CatmullRomWeights(f.y, wy0, wy1, wy2, wy3);
    float sum = 0.0;
    float wsum = 0.0;
    [unroll]
    for (int j = -1; j <= 2; ++j) {
        const float wy = (j == -1) ? wy0 : (j == 0) ? wy1 : (j == 1) ? wy2 : wy3;
        [unroll]
        for (int i = -1; i <= 2; ++i) {
            const float wx = (i == -1) ? wx0 : (i == 0) ? wx1 : (i == 1) ? wx2 : wx3;
            const float s = tex.SampleLevel(
                sam, base + float2(float(i), float(j)) / src, 0.0).r;
            sum += s * wx * wy;
            wsum += wx * wy;
        }
    }
    return sum / (abs(wsum) > 1e-6 ? wsum : 1.0);
}

float4 SampleRgbCatmullRom(Texture2D tex, SamplerState sam, float2 uv) {
    uint w, h;
    tex.GetDimensions(w, h);
    const float2 src = float2(w, h);
    const float2 d = float2(ddx(uv.x), ddy(uv.y));
    const float2 scale = float2(abs(d.x) * src.x, abs(d.y) * src.y);
    if (scale.x >= 1.0001f && scale.y >= 1.0001f) {
        return tex.SampleLevel(sam, uv, 0.0);
    }
    const float2 pos = uv * src - 0.5;
    const float2 f = frac(pos);
    const float2 base = (floor(pos) + 0.5) / src;
    float wx0, wx1, wx2, wx3;
    float wy0, wy1, wy2, wy3;
    CatmullRomWeights(f.x, wx0, wx1, wx2, wx3);
    CatmullRomWeights(f.y, wy0, wy1, wy2, wy3);
    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
    float wsum = 0.0;
    [unroll]
    for (int j = -1; j <= 2; ++j) {
        const float wy = (j == -1) ? wy0 : (j == 0) ? wy1 : (j == 1) ? wy2 : wy3;
        [unroll]
        for (int i = -1; i <= 2; ++i) {
            const float wx = (i == -1) ? wx0 : (i == 0) ? wx1 : (i == 1) ? wx2 : wx3;
            const float4 s = tex.SampleLevel(
                sam, base + float2(float(i), float(j)) / src, 0.0);
            sum += s * (wx * wy);
            wsum += wx * wy;
        }
    }
    return sum / (abs(wsum) > 1e-6 ? wsum : 1.0);
}

float4 main(VSOutput input) : SV_TARGET {
    const float peak = max(disp.x, 100.0);
    // Normalisation reference for PQ/HLG input, in nits.
    //
    // PQ/HLG decode yields absolute luminance, so this divides it down to a
    // 0..~5 range that the tone curve expects. The divisor is the *display
    // referred paper white* (adj.w, 203 nits by default), NOT the mastering
    // display max (cb[9]): ITU-R BT.2408 puts HDR reference white at 203 nits,
    // which must land near 90% of an SDR signal. Dividing by the mastering max
    // instead put 203 nits at 59% of signal -- mid-grey -- which is the
    // "HDR looks wrong / too dark" symptom. A prior revision justified
    // masterMax with an ffmpeg zscale+tonemap comparison; that reference was
    // broken (its chain omitted the output OETF, so it rendered BT.709 values
    // without a BT.709 transfer function) and the mean-error numbers it cites
    // are void. Content peak still caps the signal via min(norm, peak) below.
    const float norm = max(adj.w, 1.0);
    float3 rgb;

    // extra.y = staticRGBA (part of the audit 5.6 #9 lane repack).
    if (extra.y > 0.5) {
        float4 c = texY.Sample(sam, input.uv);
        rgb = (mode.y > 0.5) ? (PqToLinear(saturate(c.rgb)) / norm)
                             : SrgbToLinear(c.rgb);
    } else if (extra.x > 1.5) {
        // P010 (10-bit) surface. The pool mirrors the decoder surface
        // regardless of transfer, so the SIGNAL must be read from the colour
        // constants: PQ/HLG content is BT.2020 HDR; SDR transfer is ordinary
        // BT.601/709 video and follows the 8-bit path's math below.
        float  yRaw  = SampleLumaCatmullRom(texY, sam, input.uv);
        float2 uvRaw = texUV.Sample(sam, input.uv).rg;
        float y10  = yRaw * 1023.68;
        float cb10 = uvRaw.x * 1023.68;
        float cr10 = uvRaw.y * 1023.68;

        float yN, cbN, crN;
        if (mode.z > 0.5) {
            yN = y10 / 1023.0;
            cbN = cb10 / 1023.0 - 0.5;
            crN = cr10 / 1023.0 - 0.5;
        } else {
            yN  = (y10 - 64.0) / 876.0;
            cbN = (cb10 - 512.0) / 896.0;
            crN = (cr10 - 512.0) / 896.0;
        }
        float r, g, b;
        if (mode.y > 0.5) {
            // HDR (PQ/HLG): BT.2020 non-constant-luminance matrix.
            r = yN + 1.4746 * crN;
            g = yN - 0.16455 * cbN - 0.57135 * crN;
            b = yN + 1.8814 * cbN;
            rgb = float3(r, g, b);
            if (mode.y > 1.5) {
                rgb = HlgToLinear(saturate(rgb)) / norm;
            } else {
                rgb = PqToLinear(saturate(rgb)) / norm;
            }
            // Stays in the SOURCE gamut (BT.2020): the HDR->SDR tone map
            // converts the primaries itself (inside the IPT transform),
            // and the HDR-output path wants BT.2020 anyway. Converting here
            // and back later lost highlights and mismatched the swap chain's
            // declared colour space.
        } else {
            // SDR transfer: the 10-bit version of the 8-bit branch below —
            // same matrices, same linearisation (e.g. Main10/bt709 content).
            if (mode.x < 0.5) {
                r = yN + 1.402 * crN;
                g = yN - 0.344136 * cbN - 0.714136 * crN;
                b = yN + 1.772 * cbN;
            } else {
                r = yN + 1.5748 * crN;
                g = yN - 0.1873 * cbN - 0.4681 * crN;
                b = yN + 1.8556 * cbN;
            }
            rgb = SrgbToLinear(saturate(float3(r, g, b)));
        }
    } else if (extra.x > 0.5) {
        // RGB video slot (P0-3: software-decode BGRA/RGBA16 pool): already
        // RGB, skip the YUV matrix. Mirrors the static-RGBA branch above
        // plus the P010 branch's transfer split (HLG vs PQ vs SDR).
        float4 c = SampleRgbCatmullRom(texY, sam, input.uv);
        if (mode.y > 1.5) {
            rgb = HlgToLinear(saturate(c.rgb)) / norm;
        } else if (mode.y > 0.5) {
            rgb = PqToLinear(saturate(c.rgb)) / norm;
        } else {
            rgb = SrgbToLinear(c.rgb);
        }
    } else if (mode.y > 0.5) {
        float4 c = SampleRgbCatmullRom(texY, sam, input.uv);
        rgb = PqToLinear(saturate(c.rgb)) / norm;
    } else {
        float  y  = SampleLumaCatmullRom(texY, sam, input.uv);
        float2 uv = texUV.Sample(sam, input.uv).rg;

        float yN, cbN, crN;
        if (mode.z > 0.5) {
            yN = y;
            cbN = uv.x - 0.5;
            crN = uv.y - 0.5;
        } else {
            yN = (y - 16.0/255.0) * (255.0/219.0);
            cbN = (uv.x - 128.0/255.0) * (255.0/224.0);
            crN = (uv.y - 128.0/255.0) * (255.0/224.0);
        }

        float r, g, b;
        if (mode.x < 0.5) {
            r = yN + 1.402 * crN;
            g = yN - 0.344136 * cbN - 0.714136 * crN;
            b = yN + 1.772 * cbN;
        } else {
            r = yN + 1.5748 * crN;
            g = yN - 0.1873 * cbN - 0.4681 * crN;
            b = yN + 1.8556 * cbN;
        }
        // Y'CbCr -> R'G'B' is defined on *non-linear* (OETF-encoded) components,
        // so the matrix above produces gamma-encoded R'G'B', not linear light.
        // Every later stage (tone curve, output transfer) works in linear light,
        // so linearise here. Omitting this re-applied the sRGB OETF twice and
        // washed every SDR video out (source 128 rendered as 188).
        rgb = SrgbToLinear(saturate(float3(r, g, b)));
    }

    if (mode.w < 0.5) {
        // SDR output adjustments. Clamp to [0,1] only for SDR input: HDR input
        // is normalised by paper white, so 1.0 *is* paper white and anything
        // above it is legitimate highlight headroom that the tone curve has to
        // be allowed to roll off. Clamping first collapsed every value >= paper
        // white onto one code, blowing out all highlight gradation (397, 594,
        // 806 and 1004 nits all rendered identically).
        // HDR *signal* is a transfer-function property (PQ/HLG) — the pool
        // format (P010 vs NV12) says nothing about it: 10-bit SDR content
        // sits in P010 pools too.
        const bool hdrInput = (mode.y > 0.5);
        rgb = rgb * (adj.x * adj.y);
        rgb = hdrInput ? max(rgb, 0.0) : saturate(rgb);
        float luma = dot(rgb, float3(0.2126, 0.7152, 0.0722));
        rgb = lerp(luma, rgb, adj.z);
    }

    // (Removed: Y-only / UV-only debug visualization — the only reader of the
    // old debugMode lane, which had no setter and was always 0. That DWORD is
    // now reused for SDR white nits; audit 5.6 #9.)

    if (mode.w > 0.5) {
        float3 lin;
        if (mode.y > 0.5) {
            // HDR input: PQ/HLG decode already yields absolute luminance, which
            // was divided by `norm` (paper white) on the way in, so scale it
            // back and cap at the display peak.
            lin = min(rgb * min(norm, peak), peak);
        } else {
            // SDR input on an HDR output. rgb is linear with 1.0 == SDR
            // diffuse white, which is defined as 100 nits by ITU-R BT.2408 —
            // NOT the HDR reference/paper white (adj.w, 203 nits).
            //
            // Scaling by adj.w here lifted SDR white to 203 nits, ~2x too
            // bright, which reads as washed-out/milky SDR on an HDR panel.
            // Map SDR white to its own nits (extra.z) so grade and contrast
            // survive; only the tone-map TARGET (disp.x, display peak) applies
            // above it.
            const float sdrWhiteNits = extra.z > 1.0 ? extra.z : 100.0;
            lin = rgb * sdrWhiteNits;
        }
        return float4(LinearToPq((mode.y > 0.5) ? lin : mul(lin, BT709_TO_BT2020)), 1.0);
    }

    if (mode.y > 0.5) {
        // HDR content on an SDR target: mpv / libplacebo parity.
        //
        // libplacebo v7.360.0 (mpv 0.41.0) tone-maps the intensity channel
        // of the IPT colour space and keeps the chroma, so a saturated
        // highlight keeps its hue instead of being scaled per RGB channel.
        // The curve itself is the single-pivot polynomial spline whose
        // coefficients the CPU solved from the measured source peak/average
        // (HdrToneMapping.hpp).
        //
        // rgb arrives linear with 1.0 == paper white (adj.w nits) and in the
        // SOURCE gamut (BT.2020 for HDR) - the gamut conversion happens
        // below, inside the tone map, exactly like libplacebo does it.
        const float3 nits = rgb * norm;

        // 1) source gamut -> LMS -> PQ -> IPT
        const float3 lms = mul(RGB2020_TO_LMS, nits);
        const float3 lmspq = LinearToPq(lms);
        float3 ipt = mul(LMS_TO_IPT, lmspq);
        const float iOrig = ipt.x;

        // 2) the curve, on intensity only. The input is clamped to the
        // curve's domain, like libplacebo's LUT sampling does.
        ipt.x = ToneMapSpline(clamp(ipt.x, 0.00011948, extra.z));

        // 3) hue hull: do not raise saturation more than the curve's own
        // gamut allows (libplacebo's "hull" clamp).
        float2 hull = float2(iOrig, ipt.x);
        hull = ((hull - 6.0) * hull + 9.0) * hull;
        ipt.yz *= min(iOrig / max(ipt.x, 1e-6), hull.y / max(hull.x, 1e-6));

        // 4) back to linear light in the TARGET gamut (BT.709).
        // PqToLinear returns absolute nits; dividing by the SDR reference
        // white (203 cd/m²) puts the value in libplacebo's NORM scale
        // (1.0 == 203 cd/m²). (Multiplying by 10000/203 here instead would
        // be 49x too bright and clip everything to white.)
        const float3 lmsOut = PqToLinear(mul(IPT_TO_LMS, ipt)) / 203.0;
        rgb = saturate(mul(LMS_TO_RGB709, lmsOut));

        // 5) black-scaled delinearize: the target black (203/1000 cd/m²)
        // lands on code 0, the SDR reference white (203 cd/m²) on code 1 -
        // the same mapping libplacebo applies for a 1000:1 SDR target.
        const float3 signal = (rgb - 0.001) / 0.999;
        // Gamma 2.2 display transfer, matching the swap chain's
        // RGB_FULL_G22_NONE_P709 colour space.
        return float4(APPLY_DITHER(pow(saturate(signal), 1.0 / 2.2),
                                   input.pos.xy), 1.0);
    }
    return float4(APPLY_DITHER(LinearToSrgb(saturate(rgb)), input.pos.xy), 1.0);
}
)";

// ─── Peak detection (libplacebo's pl_shader_detect_peak) ─────────────────
//
// Measures the source peak and average luminance in PQ space so the tone
// curve can adapt to the content - the same measurement libplacebo takes
// before it builds its spline. Mirrors lp_sh_colorspace.c:
//
//   luma = Rec.2020 Y of the linearised RGB
//   pq   = PQ(luma), attenuated by smoothstep(0, 0.01, pq)  (libplacebo's
//          black cutoff: below it a pixel counts as black and is excluded)
//   yq   = uint(16383 * pq)          (14-bit, like libplacebo's PQ_MAX)
//
// The average is the mean over the non-black pixels, the peak the maximum.
// The pass runs on a 2-pixel stride: the average is unaffected, the peak is
// marginally conservative, and the pass costs a quarter of a full sweep -
// mpv re-runs its equivalent at full resolution every rendered frame.
static const char* g_toneStatsShaderSource = R"(
Texture2D texY : register(t0);
Texture2D texUV : register(t1);
SamplerState sam : register(s0);

cbuffer ToneStatsParams : register(b0) {
    uint4 dims;   // x=width, y=height, z=transfer (1=PQ, 2=HLG), w=flags
    float4 misc;  // x=uvScaleX, y=uvScaleY, z=inputFormat, w=unused
};

RWStructuredBuffer<uint> gStats : register(u0);   // [0]=count [1]=sum [2]=max

float3 PqToLinearNits(float3 e) {
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    float3 p = pow(max(e, 0.0), 1.0 / m2);
    float3 num = max(p - c1, 0.0);
    float3 den = c2 - c3 * p;
    return pow(num / max(den, 1e-9), 1.0 / m1) * 10000.0;
}
float LinearToPqNits(float l) {
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    float p = pow(max(l / 10000.0, 0.0), m1);
    return pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}
float3 HlgToLinearNits(float3 e) {
    const float a = 0.17883277;
    const float b = 0.28466892;
    const float c = 0.55991073;
    float3 l = (e <= 0.5) ? (e * e) / 3.0 : (exp((e - c) / a) + b) / 12.0;
    return pow(l, 1.2) * 1000.0;
}

[numthreads(8,8,1)]
void main(uint3 tid : SV_DispatchThreadID) {
    const uint x = tid.x * 2u;
    const uint y = tid.y * 2u;
    if (x >= dims.x || y >= dims.y) return;

    const float2 uv = (float2(x, y) + 0.5) / float2(dims.x, dims.y) *
                      float2(misc.x, misc.y);

    float3 nits;
    if (misc.z > 0.5 && misc.z < 1.5) {
        // RGB slot: already RGB. SampleLevel, not Sample - a compute shader
        // has no derivatives.
        const float4 c = texY.SampleLevel(sam, uv, 0.0);
        nits = (dims.z == 2u) ? HlgToLinearNits(saturate(c.rgb))
                              : PqToLinearNits(saturate(c.rgb));
    } else {
        // NV12 / P010 planes.
        const float yRaw = texY.SampleLevel(sam, uv, 0.0).r;
        const float2 uvRaw = texUV.SampleLevel(sam, uv, 0.0).rg;
        float yN, cbN, crN;
        if ((dims.w & 2u) != 0u) {
            const float y10 = yRaw * 1023.68;
            const float cb10 = uvRaw.x * 1023.68;
            const float cr10 = uvRaw.y * 1023.68;
            if ((dims.w & 1u) != 0u) {
                yN = y10 / 1023.0;  cbN = cb10 / 1023.0 - 0.5;
                crN = cr10 / 1023.0 - 0.5;
            } else {
                yN = (y10 - 64.0) / 876.0;  cbN = (cb10 - 512.0) / 896.0;
                crN = (cr10 - 512.0) / 896.0;
            }
        } else {
            if ((dims.w & 1u) != 0u) {
                yN = yRaw;  cbN = uvRaw.x - 0.5;  crN = uvRaw.y - 0.5;
            } else {
                yN = (yRaw - 16.0/255.0) * (255.0/219.0);
                cbN = (uvRaw.x - 128.0/255.0) * (255.0/224.0);
                crN = (uvRaw.y - 128.0/255.0) * (255.0/224.0);
            }
        }
        // BT.2020 non-constant-luminance matrix (HDR is always BT.2020).
        const float3 pq = saturate(float3(
            yN + 1.4746 * crN,
            yN - 0.16455 * cbN - 0.57135 * crN,
            yN + 1.8814 * cbN));
        nits = (dims.z == 2u) ? HlgToLinearNits(pq) : PqToLinearNits(pq);
    }

    // Rec.2020 luminance -> PQ -> black cutoff -> 14-bit code.
    const float luma = dot(nits, float3(0.2627, 0.6780, 0.0593));
    float pq = LinearToPqNits(luma);
    pq *= smoothstep(0.0, 0.01, pq);
    const uint yq = (uint)(16383.0 * pq);
    if (yq == 0u) return;   // below the black cutoff: not a signal pixel

    InterlockedAdd(gStats[0], 1u);
    InterlockedAdd(gStats[1], yq);
    InterlockedMax(gStats[2], yq);
}
)";

// ─── Peak detection pass ─────────────────────────────────────────────────

bool D3D12Renderer::CreateToneStatsPipeline() {
    ID3D12Device* dev = m_device.Get();
    if (!dev) return false;

    // ── Root signature: 2 SRVs (t0/t1), 1 UAV (u0), 8 constants (b0) ──
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors     = 2;
    srvRange.BaseShaderRegister = 0;
    srvRange.RegisterSpace      = 0;

    D3D12_DESCRIPTOR_RANGE uavRange = {};
    uavRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors     = 1;
    uavRange.BaseShaderRegister = 0;
    uavRange.RegisterSpace      = 0;

    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges   = &srvRange;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // The UAV is bound through a descriptor table (the SDK's
    // SetComputeRootUnorderedAccessView only takes a virtual address).
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges   = &uavRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[2].ParameterType                    = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants.ShaderRegister         = 0;
    params[2].Constants.RegisterSpace          = 0;
    params[2].Constants.Num32BitValues         = 8;
    params[2].ShaderVisibility                 = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter         = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD         = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters     = 3;
    desc.pParameters       = params;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers   = &sampler;

    Microsoft::WRL::ComPtr<ID3DBlob> sigBlob, errorBlob;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                              &sigBlob, &errorBlob);
    if (FAILED(hr)) {
        REX_LOG_ERROR("D3D12", "Tone stats root signature failed: {:08x}", hr);
        return false;
    }
    m_toneStatsRootSignature = m_device.CreateRootSignature(
        sigBlob->GetBufferPointer(), sigBlob->GetBufferSize());
    if (!m_toneStatsRootSignature) return false;

    // ── Compute pipeline ──
    Microsoft::WRL::ComPtr<ID3DBlob> csBlob, csError;
    hr = D3DCompile(g_toneStatsShaderSource, strlen(g_toneStatsShaderSource),
                    nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0,
                    &csBlob, &csError);
    if (FAILED(hr)) {
        REX_LOG_ERROR("D3D12", "Tone stats shader compile failed: {:08x} {}",
                      hr, csError ? (const char*)csError->GetBufferPointer() : "");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = m_toneStatsRootSignature.Get();
    psoDesc.CS.pShaderBytecode = csBlob->GetBufferPointer();
    psoDesc.CS.BytecodeLength  = csBlob->GetBufferSize();
    Microsoft::WRL::ComPtr<ID3D12PipelineState> computePso;
    hr = dev->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&computePso));
    if (FAILED(hr) || !computePso) {
        REX_LOG_ERROR("D3D12", "Tone stats pipeline state creation failed: {:08x}", hr);
        return false;
    }
    m_toneStatsPipelineState = computePso;

    // ── Per-frame-context resources ──
    // Two buffers per context: the UAV the compute shader accumulates into
    // (default heap) and the copy the CPU reads back (readback heap).
    D3D12_RESOURCE_DESC uavRes = {};
    uavRes.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    uavRes.Width            = 16;   // 4 uints: count, sum, max, spare
    uavRes.Height           = 1;
    uavRes.DepthOrArraySize = 1;
    uavRes.MipLevels        = 1;
    // DXGI_FORMAT_UNKNOWN, not R32_UINT: this driver rejects a UAV buffer
    // with a typed format (E_INVALIDARG) and wants a raw buffer - the UAV
    // view still describes the contents as R32_UINT below.
    uavRes.Format           = DXGI_FORMAT_UNKNOWN;
    uavRes.SampleDesc.Count = 1;
    uavRes.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;   // buffers
    uavRes.Flags            = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_RESOURCE_DESC readbackRes = uavRes;
    readbackRes.Format = DXGI_FORMAT_UNKNOWN;
    readbackRes.Flags  = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES defaultHeap = {};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES readbackHeap = {};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;

    // The UAV descriptors live in the shared CBV_SRV_UAV heap, in their own
    // half past the static texture (slot 0) and the video slots (1..kMaxVideoSlots*2).
    const UINT uavBase = kToneStatsUavBase;
    const UINT inc = dev->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpuBase = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpuBase = m_srvHeap->GetGPUDescriptorHandleForHeapStart();

    for (UINT i = 0; i < graphics::kFrameContextCount; ++i) {
        ToneStats& st = m_toneStats[i];
        st.buffer = m_device.CreateCommittedResource(
            defaultHeap, D3D12_HEAP_FLAG_NONE, uavRes,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr);
        if (!st.buffer) {
            REX_LOG_ERROR("D3D12", "Tone stats buffer creation failed");
            return false;
        }

        st.readback = m_device.CreateCommittedResource(
            readbackHeap, D3D12_HEAP_FLAG_NONE, readbackRes,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr);
        if (!st.readback) {
            REX_LOG_ERROR("D3D12", "Tone stats readback creation failed");
            return false;
        }
        void* mapped = nullptr;
        if (FAILED(st.readback->Map(0, nullptr, &mapped)) || !mapped) {
            REX_LOG_ERROR("D3D12", "Tone stats readback map failed");
            return false;
        }
        st.mapped = static_cast<uint32_t*>(mapped);
        ZeroMemory(st.mapped, 16);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format           = DXGI_FORMAT_R32_UINT;
        uavDesc.ViewDimension    = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.NumElements = 4;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = cpuBase;
        cpu.ptr += (uavBase + i) * inc;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = gpuBase;
        gpu.ptr += (uavBase + i) * inc;
        dev->CreateUnorderedAccessView(st.buffer.Get(), nullptr, &uavDesc, cpu);
        st.uavCpu = cpu;
        st.uavGpu = gpu;
    }

    m_toneStatsReady = true;
    REX_LOG_INFO("D3D12", "Peak-detection pipeline ready (mpv parity)");
    return true;
}

void D3D12Renderer::DispatchToneStats(UINT contextIndex,
                                       D3D12_GPU_DESCRIPTOR_HANDLE srvGpu) {
    if (!m_toneStatsReady || contextIndex >= graphics::kFrameContextCount) return;
    ToneStats& st = m_toneStats[contextIndex];
    if (!st.buffer || !st.readback) return;

    // Re-measure only when the decoded frame changed. The smoothed state
    // still advances once per rendered frame, so the curve converges exactly
    // like mpv's (which re-measures the same frame on every render).
    if (m_statsFrameIndex == m_activeFrameIndex) return;
    m_statsFrameIndex = m_activeFrameIndex;

    const UINT width  = static_cast<UINT>(m_videoWidth);
    const UINT height = static_cast<UINT>(m_videoHeight);
    if (width == 0 || height == 0) return;

    // ── Parameters ──
    UINT flags = 0;
    if (m_activeColorInfo.fullRange) flags |= 1u;
    if (m_inputFormat > 1.5f) flags |= 2u;   // P010: 10-bit planes
    const bool hlg = m_activeColorInfo.transfer == VideoColorInfo::Transfer::HLG;

    struct ToneStatsConstants {
        UINT dims[4];    // x=width, y=height, z=transfer, w=flags
        float misc[4];   // x=uvScaleX, y=uvScaleY, z=inputFormat
    } params = {};
    params.dims[0] = width;
    params.dims[1] = height;
    params.dims[2] = hlg ? 2u : 1u;
    params.dims[3] = flags;
    params.misc[0] = m_videoUvScaleX;
    params.misc[1] = m_videoUvScaleY;
    params.misc[2] = m_inputFormat;

    // ── Clear the accumulator ──
    const UINT zeros[4] = {0, 0, 0, 0};
    m_commandList->ClearUnorderedAccessViewUint(st.uavGpu, st.uavCpu,
                                                 st.buffer.Get(), zeros, 0, nullptr);

    // ── Dispatch ──
    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
    m_commandList->SetDescriptorHeaps(1, heaps);
    m_commandList->SetComputeRootSignature(m_toneStatsRootSignature.Get());
    m_commandList->SetPipelineState(m_toneStatsPipelineState.Get());
    m_commandList->SetComputeRootDescriptorTable(0, srvGpu);
    m_commandList->SetComputeRootDescriptorTable(1, st.uavGpu);
    m_commandList->SetComputeRoot32BitConstants(2, 8, &params, 0);

    // Each thread samples one pixel of a 2-pixel stride; 8 threads cover
    // 16 pixels.
    const UINT groupsX = (width + 15u) / 16u;
    const UINT groupsY = (height + 15u) / 16u;
    m_commandList->Dispatch(groupsX, groupsY, 1);

    // ── Copy the result out for the CPU ──
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = st.buffer.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &barrier);

    m_commandList->CopyBufferRegion(st.readback.Get(), 0, st.buffer.Get(), 0, 16);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    m_commandList->ResourceBarrier(1, &barrier);

    st.pending = true;
    if (!m_loggedStatsDispatch) {
        m_loggedStatsDispatch = true;
        REX_LOG_INFO("D3D12", "tone stats dispatched: frame={} {}x{} fmt={}",
                     m_activeFrameIndex, width, height, m_inputFormat);
    }
}

void D3D12Renderer::ConsumeToneStats(UINT contextIndex) {
    if (!m_toneStatsReady || contextIndex >= graphics::kFrameContextCount) return;
    ToneStats& st = m_toneStats[contextIndex];
    if (!st.pending || !st.mapped) return;
    st.pending = false;

    const uint32_t count = st.mapped[0];
    const uint32_t sum   = st.mapped[1];
    const uint32_t peak  = st.mapped[2];
    if (!m_loggedStatsRead) {
        m_loggedStatsRead = true;
        REX_LOG_INFO("D3D12", "tone stats read: count={} sum={} peak={}", count, sum, peak);
    }
    if (count == 0) return;   // an all-black frame carries no measurement

    constexpr float kPqScale = 16383.0f;   // libplacebo's 14-bit PQ_MAX
    m_measuredMaxPq = static_cast<float>(peak) / kPqScale;
    m_measuredAvgPq = static_cast<float>(sum) / (static_cast<float>(count) * kPqScale);
}

// ─── Constructor / Destructor ──────────────────────────────────────────────

D3D12Renderer::~D3D12Renderer() {
    try {
        Shutdown();
    } catch (const std::exception& ex) {
        REX_LOG_ERROR("D3D12", "~D3D12Renderer exception: {}", ex.what());
    } catch (...) {
        REX_LOG_ERROR("D3D12", "~D3D12Renderer unknown exception");
    }
}

// ─── Initialize ───────────────────────────────────────────────────────────

shared::Result<void> D3D12Renderer::Initialize(HWND targetWindow, int width, int height) {
    return Initialize(targetWindow, width, height, nullptr);
}

shared::Result<void> D3D12Renderer::Initialize(HWND targetWindow, int width, int height,
                                               ID3D12Device* adoptDevice) {
    return InitializeCore(targetWindow, width, height, adoptDevice, nullptr);
}

// Single init core (audit 5.6 #11): the adopted-device overload and the
// shared-queue overload used to carry ~200 near-identical lines. The only
// differences are the local command queue (skipped when the compositor owns
// a shared one) and the log wording.
shared::Result<void> D3D12Renderer::InitializeCore(HWND targetWindow, int width, int height,
                                                   ID3D12Device* adoptDevice,
                                                   graphics::CommandQueue* sharedQueue) {
    if (m_initialized) {
        return shared::Result<void>(shared::ErrorCode::AlreadyInitialized,
                                    "Renderer already initialized");
    }

    REX_LOG_INFO("D3D12", "Initializing D3D12 renderer ({}x{}{}{})", width, height,
                 adoptDevice ? " on adopted device" : "",
                 sharedQueue ? ", shared queue" : "");

    m_width  = width;
    m_height = height;
    // Non-owning; ActiveQueue() prefers it over the local queue created below.
    if (sharedQueue) m_sharedQueue = sharedQueue;

    // ── Device ─────────────────────────────────────────────────────────
    auto devResult = adoptDevice ? m_device.InitializeAdopted(adoptDevice)
                                 : m_device.Initialize();
    if (devResult.IsError()) {
        return devResult;
    }

    // ── Command Queue ──────────────────────────────────────────────────
    // A shared queue is owned by the compositor: never initialize a local one.
    if (!sharedQueue) {
        auto qResult = m_commandQueue.Initialize(m_device.Get());
        if (qResult.IsError()) {
            return qResult;
        }
    }

    // ── Frame-context ring (P0-4): one allocator per in-flight frame ──
    for (UINT i = 0; i < graphics::kFrameContextCount; ++i) {
        m_frameContexts[i].allocator = m_device.CreateCommandAllocator();
        if (!m_frameContexts[i].allocator) {
            return shared::Result<void>(shared::ErrorCode::UnknownError,
                                        "Failed to create command allocator");
        }
        m_frameContexts[i].fenceValue = 0;
    }
    m_frameNumber = 0;

    // ── Swap Chain ─────────────────────────────────────────────────────
    // --force-sdr: lock swapchain to SDR format before first frame
    if (m_forceSdr) {
        m_swapChain.SetForceSdr(true);
    }
    auto scResult = m_swapChain.Initialize(
        ActiveQueue().Get(), m_device.Get(),
        targetWindow, width, height);
    if (scResult.IsError()) {
        return scResult;
    }

    // ── SRV heap (shader-visible, 128 slots) ──────────────────────────
    m_srvHeap = m_device.CreateDescriptorHeap(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvHeapSize,
        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
    if (!m_srvHeap) {
        return shared::Result<void>(shared::ErrorCode::OutOfMemory,
                                    "Failed to create SRV heap");
    }

    // ── Root signature ────────────────────────────────────────────────
    if (!CreateRootSignature()) {
        return shared::Result<void>(shared::ErrorCode::UnknownError,
                                    "Failed to create root signature");
    }

    // ── Peak detection (tone curve input) ────────────────────────────
    // Not fatal: without it the tone curve falls back to a static source
    // peak, the way the engine behaved before mpv parity.
    CreateToneStatsPipeline();
    // Until the first measurement lands, assume a 1000 cd/m² source - the
    // same fallback the engine used before, and close to what most HDR
    // content actually peaks at.
    m_toneCurve = tone::BuildSplineCurve(tone::PqEncode(1000.0f), 0.0f);

    // ── Vertex buffer ─────────────────────────────────────────────────
    if (!CreateVertexBuffer()) {
        return shared::Result<void>(shared::ErrorCode::UnknownError,
                                    "Failed to create vertex buffer");
    }

    // ── Pipeline state ────────────────────────────────────────────────
    // One PSO per back-buffer format: SDR (10-bit UNORM, 8-bit on a driver
    // that refuses it) and HDR (16-bit float). The shader branches on the
    // HDR flag via root constants; the dither amplitude is compiled in
    // from the target depth.
    m_sdrPsoFormat = m_swapChain.GetBackBufferFormat();
    if (!CreatePipelineState(m_sdrPsoFormat, m_pipelineState)) {
        return shared::Result<void>(shared::ErrorCode::UnknownError,
                                    "Failed to create SDR pipeline state");
    }
    m_isHdrOutput = m_swapChain.IsHDR();
    if (m_isHdrOutput) {
        if (!CreatePipelineState(DXGI_FORMAT_R16G16B16A16_FLOAT, m_pipelineStateHdr)) {
            return shared::Result<void>(shared::ErrorCode::UnknownError,
                                        "Failed to create HDR pipeline state");
        }
    }

    // ── Initialize command list ───────────────────────────────────────
    m_commandList = m_device.CreateCommandList(
        m_frameContexts[0].allocator.Get(), m_pipelineState.Get());
    if (!m_commandList) {
        return shared::Result<void>(shared::ErrorCode::UnknownError,
                                    "Failed to create command list");
    }
    m_commandList->Close();

    // ── Pre-create copy/upload context ring (P5) ──────────────────────
    // Steady-state uploads must not allocate command buffers. Lazy
    // creation inside AcquireCopyContext only fires if these fail below.
    for (UINT i = 0; i < kCopyContextCount; ++i) {
        m_copyContexts[i].allocator = m_device.CreateCommandAllocator();
        if (!m_copyContexts[i].allocator) {
            return shared::Result<void>(shared::ErrorCode::UnknownError,
                                        "Failed to create copy allocator");
        }
        m_copyContexts[i].list =
            m_device.CreateCommandList(m_copyContexts[i].allocator.Get());
        if (!m_copyContexts[i].list) {
            return shared::Result<void>(shared::ErrorCode::UnknownError,
                                        "Failed to create copy list");
        }
        m_copyContexts[i].list->Close(); // created open; Reset() needs closed
    }

    // (Removed: upload-heap audio constant buffer — audit 5.6 #11. RenderFrame
    // uploads b2 straight from m_currentSpectrum as root constants; the
    // mapped CB was never read by any draw.)

    m_initialized = true;
    REX_LOG_INFO("D3D12", "Renderer initialized successfully");
    return shared::Result<void>();
}

shared::Result<void> D3D12Renderer::Initialize(HWND targetWindow, int width, int height,
                                               ID3D12Device* adoptDevice,
                                               graphics::CommandQueue* sharedQueue) {
    if (!sharedQueue || !sharedQueue->Get()) {
        return shared::Result<void>(shared::ErrorCode::InvalidArgument,
                                    "shared queue is null");
    }
    // Single init core (audit 5.6 #11): see InitializeCore.
    return InitializeCore(targetWindow, width, height, adoptDevice, sharedQueue);
}

void D3D12Renderer::Shutdown() {
    if (!m_initialized) return;

    REX_LOG_INFO("D3D12", "Shutting down renderer...");

    std::lock_guard<std::mutex> lock(m_renderMutex);
    ActiveQueue().WaitForGPU();

    // Release wallpaper-specific resources
    m_texture.Reset();
    m_textureUploadBuffer.Reset();
    m_hasTexture = false;
    for (auto& slot : m_videoSlots) {
        slot.resource.Reset();
        slot.sharedHandle = nullptr;
        slot.srvGpu = {};
    }
    m_videoSlotCount = 0;
    m_activeVideoHandle = nullptr;
    m_activePoolGeneration = 0;
    m_hasVideoFrame = false;

    // Unmap audio spectrum CB (removed with the CB itself — audit 5.6 #11)
    m_currentSpectrum.fill(0.0f);

    m_activeColorInfo = VideoColorInfo{};
    m_inputFormat = 0.0f;
    m_vertexBuffer.Reset();
    m_pipelineState.Reset();
    m_pipelineStateHdr.Reset();
    m_customPSO.Reset();
    m_customPSOHdr.Reset();
    m_customPsActive = false;
    m_rootSignature.Reset();

    // Peak-detection resources (tone curve input).
    for (auto& st : m_toneStats) {
        if (st.readback && st.mapped) {
            st.readback->Unmap(0, nullptr);
            st.mapped = nullptr;
        }
        st.buffer.Reset();
        st.readback.Reset();
        st.pending = false;
    }
    m_toneStatsPipelineState.Reset();
    m_toneStatsRootSignature.Reset();
    m_toneStatsReady = false;
    m_peakState.Reset();
    m_statsFrameIndex = UINT64_MAX;

    m_srvHeap.Reset();
    m_commandList.Reset();
    for (auto& ctx : m_frameContexts) {
        ctx.allocator.Reset();
        ctx.fenceValue = 0;
    }
    m_frameNumber = 0;

    // Shutdown shared primitives (order: swap chain → queue → device)
    m_swapChain.Shutdown();
    if (!m_sharedQueue) {
        m_commandQueue.Shutdown();
    }
    // Device has no persistent state to clean up

    m_initialized = false;
    // Clear the loss flag with the device it described. It used to survive
    // Shutdown(), so a shut-down renderer kept reporting "device removed"
    // forever and the engine re-entered device-loss recovery every single
    // frame when the desktop host could not be reacquired.
    m_deviceRemoved = false;
    REX_LOG_INFO("D3D12", "Renderer shutdown complete");
}

// ─── Texture Loading ──────────────────────────────────────────────────────

shared::Result<void> D3D12Renderer::LoadTexture(std::string_view path) {
    // R1: never lock the frame mutex on the frame owner thread inside a
    // frame — std::mutex is non-recursive, so that self-deadlocks. Fail
    // loudly; cross-thread callers still block normally for exclusion.
    if (m_frameLock.IsReentrantCall()) {
        return shared::Result<void>(shared::ErrorCode::InvalidState,
                                    "LoadTexture called inside an open frame "
                                    "on the frame owner thread");
    }
    if (!m_initialized) {
        return shared::Result<void>(shared::ErrorCode::NotInitialized,
                                    "Renderer not initialized");
    }

    REX_LOG_INFO("D3D12", "Loading texture: {}", path);

    TextureData texData = DecodeTextureFromFile(path);
    if (texData.pixels.empty()) {
        return shared::Result<void>(shared::ErrorCode::InvalidFormat,
                                    "WIC decode failed: " + std::string(path));
    }

    std::lock_guard<std::mutex> lock(m_renderMutex);

    if (m_texture) {
        ActiveQueue().WaitForGPU();
        m_texture.Reset();
        m_hasTexture = false;
    }

    if (!UploadTextureToGPU(texData)) {
        return shared::Result<void>(shared::ErrorCode::OutOfMemory,
                                    "GPU texture upload failed");
    }

    REX_LOG_INFO("D3D12", "Texture loaded: {}x{}", texData.width, texData.height);
    return shared::Result<void>();
}

void D3D12Renderer::ClearTexture() {
    std::lock_guard<std::mutex> lock(m_renderMutex);
    if (m_texture) {
        ActiveQueue().WaitForGPU();
        m_texture.Reset();
        m_textureUploadBuffer.Reset();
        m_hasTexture = false;
        m_inputFormat = 0.0f;
    }
}

// ─── Video (shared-handle) frames ─────────────────────────────────────────

shared::Result<void> D3D12Renderer::DrawVideoFrame(const VideoFrame& frame,
                                                   ID3D12Fence* waitFence) {
    if (!m_initialized) {
        return shared::Result<void>(shared::ErrorCode::NotInitialized,
                                    "Renderer not initialized");
    }
    if (!frame.valid || !frame.sharedHandle) {
        return shared::Result<void>(shared::ErrorCode::InvalidArgument,
                                    "Invalid video frame");
    }

    // NOTE: no m_renderMutex lock here on purpose — this is called between
    // BeginFrame() and EndFrame(), which already hold the mutex (same
    // convention as UpdateTexturePixels).

    ID3D12Device* dev = m_device.Get();

    // Locate or open the pool slot resource. Handles are stable in the
    // steady state, but a mid-session pool rebuild (stream switch / format
    // change) retires all 8 inside one video open — so the cache tracks
    // usage and retires the least-recently-drawn slot instead of failing
    // at kMaxVideoSlots (audit 2026-10-17, §5.2 #3 / F1).
    //
    // The cache holds several pool generations at once (the decoder keeps
    // retired generations alive so a format switch back reuses them). An
    // import therefore identifies itself by (handle, pool generation): the
    // kernel recycles NT handle values once closed, and matching the handle
    // alone would alias an import from a destroyed generation.
    UINT idx = kMaxVideoSlots;
    for (UINT i = 0; i < m_videoSlotCount; ++i) {
        if (m_videoSlots[i].sharedHandle == frame.sharedHandle &&
            m_videoSlots[i].poolGeneration == frame.poolGeneration) {
            idx = i;
            break;
        }
    }
    if (idx < kMaxVideoSlots) {
        m_videoSlots[idx].lastUsedTick = ++m_slotUseCounter;
    }
    if (idx == kMaxVideoSlots) {
        if (m_videoSlotCount >= kMaxVideoSlots) {
            // Drain first: D3D12 does not refcount resources for the GPU,
            // and every submitted list referencing a victim has completed
            // once the queue is idle. This runs between BeginFrame/EndFrame
            // before the pass records its video draw, so the open command
            // list cannot reference a victim either.
            ActiveQueue().WaitForGPU();
            UINT victim = kMaxVideoSlots;
            UINT64 oldest = UINT64_MAX;
            for (UINT i = 0; i < m_videoSlotCount; ++i) {
                // Keep the presented frame: with 8 slots and one active
                // handle there is always a non-active candidate.
                if (m_videoSlots[i].sharedHandle == m_activeVideoHandle) {
                    continue;
                }
                if (m_videoSlots[i].lastUsedTick < oldest) {
                    oldest = m_videoSlots[i].lastUsedTick;
                    victim = i;
                }
            }
            if (victim == kMaxVideoSlots) {
                return shared::Result<void>(shared::ErrorCode::InvalidState,
                                            "Video slot cache exhausted");
            }
            REX_LOG_INFO("D3D12", "video slot cache full - evicting slot {} "
                         "(stale pool generation)", victim);
            m_videoSlots[victim].resource.Reset();
            m_videoSlots[victim].sharedHandle = nullptr;
            m_videoSlots[victim].poolGeneration = 0;
            m_videoSlots[victim].srvGpu = {};
            m_videoSlots[victim].texWidth = 0;
            m_videoSlots[victim].texHeight = 0;
            idx = victim;
        } else {
            idx = m_videoSlotCount;
        }
        HRESULT hr = dev->OpenSharedHandle(frame.sharedHandle,
                                           IID_PPV_ARGS(&m_videoSlots[idx].resource));
        if (FAILED(hr)) {
            if (IsDeviceRemovedHr(hr)) m_deviceRemoved = true;
            return shared::Result<void>(shared::ErrorCode::DeviceLost,
                                        "OpenSharedHandle failed");
        }
        m_videoSlots[idx].sharedHandle = frame.sharedHandle;
        m_videoSlots[idx].poolGeneration = frame.poolGeneration;
        m_videoSlots[idx].lastUsedTick = ++m_slotUseCounter;

        const DXGI_FORMAT fmt = m_videoSlots[idx].resource->GetDesc().Format;
        const D3D12_RESOURCE_DESC resDesc = m_videoSlots[idx].resource->GetDesc();
        m_videoSlots[idx].texWidth = static_cast<UINT>(resDesc.Width);
        m_videoSlots[idx].texHeight = resDesc.Height;

        // Log full resource descriptor for P010 diagnostics.
        if (fmt == DXGI_FORMAT_P010 || fmt == DXGI_FORMAT_NV12) {
            const char* fmtName = (fmt == DXGI_FORMAT_P010) ? "P010" : "NV12";
            REX_LOG_INFO("D3D12", "shared resource: fmt={} {}x{} "
                         "arraySize={} mipLevels={} sampleCount={} "
                         "dimension={} flags=0x{:x}",
                         fmtName, resDesc.Width, resDesc.Height,
                         resDesc.DepthOrArraySize, resDesc.MipLevels,
                         resDesc.SampleDesc.Count,
                         static_cast<int>(resDesc.Dimension),
                         static_cast<unsigned>(resDesc.Flags));
        }

        // (Shader input format is set once, after this branch, from the bound
        // slot's actual format — a cache hit never re-enters this block.)

        const UINT inc = dev->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu =
            m_srvHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE gpu =
            m_srvHeap->GetGPUDescriptorHandleForHeapStart();

        if (fmt == DXGI_FORMAT_NV12 || fmt == DXGI_FORMAT_P010) {
            // NV12/P010: create two SRVs — t0 = Y plane, t1 = UV plane.
            // Descriptor layout: Y at (kVideoSrvBase + idx*2), UV one past.
            const bool is10bit = (fmt == DXGI_FORMAT_P010);
            const UINT base = kVideoSrvBase + idx * kVideoSrvPerSlot;

            D3D12_SHADER_RESOURCE_VIEW_DESC srvY = {};
            srvY.Format = is10bit ? DXGI_FORMAT_R16_UNORM
                                  : DXGI_FORMAT_R8_UNORM;
            srvY.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvY.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvY.Texture2D.MipLevels = 1;
            srvY.Texture2D.PlaneSlice = 0;
            D3D12_CPU_DESCRIPTOR_HANDLE cpuY = cpu;
            cpuY.ptr += base * inc;
            dev->CreateShaderResourceView(m_videoSlots[idx].resource.Get(),
                                          &srvY, cpuY);

            D3D12_SHADER_RESOURCE_VIEW_DESC srvUV = {};
            srvUV.Format = is10bit ? DXGI_FORMAT_R16G16_UNORM
                                   : DXGI_FORMAT_R8G8_UNORM;
            srvUV.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvUV.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvUV.Texture2D.MipLevels = 1;
            srvUV.Texture2D.PlaneSlice = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE cpuUV = cpu;
            cpuUV.ptr += (base + 1) * inc;
            dev->CreateShaderResourceView(m_videoSlots[idx].resource.Get(),
                                          &srvUV, cpuUV);

            // GPU handle points to Y descriptor (UV is next descriptor)
            m_videoSlots[idx].srvGpu = gpu;
            m_videoSlots[idx].srvGpu.ptr += base * inc;
        } else {
            // BGRA / RGBA16F: t0 = main SRV, t1 = dummy (shader samples texUV
            // only in NV12 mode; for RGBA16F the second descriptor must still
            // be valid to satisfy the root signature's 2-descriptor range).
            const UINT base = kVideoSrvBase + idx * kVideoSrvPerSlot;

            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc =
                MakeVideoSlotSrvDescriptor(fmt);
            D3D12_CPU_DESCRIPTOR_HANDLE cpuSrv = cpu;
            cpuSrv.ptr += base * inc;
            dev->CreateShaderResourceView(m_videoSlots[idx].resource.Get(),
                                          &srvDesc, cpuSrv);

            // Dummy t1 descriptor (same resource, default mapping)
            D3D12_SHADER_RESOURCE_VIEW_DESC dummy = srvDesc;
            dummy.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            D3D12_CPU_DESCRIPTOR_HANDLE cpuDummy = cpu;
            cpuDummy.ptr += (base + 1) * inc;
            dev->CreateShaderResourceView(m_videoSlots[idx].resource.Get(),
                                          &dummy, cpuDummy);

            m_videoSlots[idx].srvGpu = gpu;
            m_videoSlots[idx].srvGpu.ptr += base * inc;
        }

        // Reusing a victim keeps the count; an append extends it (the old
        // unconditional ++ would have drifted past kMaxVideoSlots once
        // eviction reuses an index below the count).
        if (idx >= m_videoSlotCount) m_videoSlotCount = idx + 1;
    }

    // Shader input format for the bound slot (0 = NV12, 1 = RGBA16/other,
    // 2 = P010), taken from the resource actually bound. This must run on
    // BOTH paths: a cache hit (pool generation reused after a format switch)
    // would otherwise keep the previous content's format constant and shade
    // NV12 planes as P010 (or vice versa).
    {
        const DXGI_FORMAT slotFmt = m_videoSlots[idx].resource->GetDesc().Format;
        m_inputFormat = (slotFmt == DXGI_FORMAT_P010) ? 2.0f
                      : (slotFmt == DXGI_FORMAT_NV12) ? 0.0f
                                                      : 1.0f;
    }

    // GPU ordering: the D3D11 video engine blts into this pool slot and
    // signals the shared fence; the D3D12 queue must wait on that signal
    // before the draw in RenderFrame() samples the slot.
    if (waitFence) {
        ActiveQueue().WaitForFence(waitFence, frame.fenceValue);
    }

    m_activeVideoHandle = frame.sharedHandle;
    m_activePoolGeneration = frame.poolGeneration;
    m_hasVideoFrame = true;
    m_videoWidth  = frame.width;
    m_videoHeight = frame.height;
    m_activeColorInfo = frame.colorInfo;
    // The peak-detection pass re-measures only when the decoded frame
    // changes; this is the key that tells it so.
    m_activeFrameIndex = frame.frameIndex;

    // Crop scale: sample only the active picture, never driver padding.
    // Pool textures may be larger than the frame (e.g. 1920x1152 HW
    // surface for 1920x1080 content); sampling 0..1 raw would present
    // zeroed padding rows as a green band and squeeze the picture.
    // The single scale must satisfy BOTH planes under linear filtering:
    // the binding constraint is the half-resolution chroma plane, whose
    // last active row center sits a full luma texel inside the edge.
    // (active-2)/tex keeps every sample footprint inside active rows on
    // both planes (cost: ~2 edge content rows stretched over, invisible).
    // Exact-fit textures keep exactly 1.0 (no behavior change there).
    m_videoUvScaleX = 1.0f;
    m_videoUvScaleY = 1.0f;
    for (UINT i = 0; i < m_videoSlotCount; ++i) {
        if (m_videoSlots[i].sharedHandle == frame.sharedHandle &&
            m_videoSlots[i].poolGeneration == frame.poolGeneration &&
            m_videoSlots[i].texWidth > 0 && m_videoSlots[i].texHeight > 0 &&
            frame.width > 0 && frame.height > 0) {
            if (static_cast<UINT>(frame.width) == m_videoSlots[i].texWidth) {
                m_videoUvScaleX = 1.0f;
            } else {
                m_videoUvScaleX = (static_cast<float>(frame.width) - 2.0f) /
                                  static_cast<float>(m_videoSlots[i].texWidth);
            }
            if (static_cast<UINT>(frame.height) == m_videoSlots[i].texHeight) {
                m_videoUvScaleY = 1.0f;
            } else {
                m_videoUvScaleY = (static_cast<float>(frame.height) - 2.0f) /
                                  static_cast<float>(m_videoSlots[i].texHeight);
            }
            break;
        }
    }

    // Tone-mapping target: the actual display peak when HDR is active,
    // otherwise SDR reference white (80 nits).
    float peak = m_swapChain.IsHDRActive() ? m_swapChain.GetDisplayMaxNits() : 80.0f;
    if (peak <= 0.0f) peak = 1000.0f;  // common HDR display fallback
    m_displayPeakNits = peak;
    return shared::Result<void>();
}

void D3D12Renderer::ClearVideoFrame() {
    std::lock_guard<std::mutex> lock(m_renderMutex);

    // The imported slot cache is deliberately retained. The decoder keeps
    // its pool generation across video sessions, so the next session brings
    // back the same NT handles and DrawVideoFrame hits this cache instead of
    // opening them again. Re-importing on every session charged the AMD
    // driver a permanent dedicated-VRAM footprint per import generation
    // (measured: a full 4K pool per OpenSharedHandle cycle, freed only at
    // process exit — releasing imports, trimming and destroying both
    // devices does not return it). Slots are released in Shutdown() and
    // retired by the LRU path when a different pool generation arrives.
    // Nothing is released here, so no GPU drain is needed.
    m_activeVideoHandle = nullptr;
    m_activePoolGeneration = 0;
    m_hasVideoFrame = false;
    m_videoWidth  = 0;
    m_videoHeight = 0;
    m_videoUvScaleX = 1.0f;
    m_videoUvScaleY = 1.0f;
    m_activeColorInfo = VideoColorInfo{};
    m_inputFormat = 0.0f;
}

// ─── Update Texture Pixels (debug / experiment path) ─────────────────────
// No in-tree callers (audit 5.6 #10): per-frame video goes through
// DrawVideoFrame, static images through LoadTexture. Each call ends in a
// full WaitForGPU, so wiring this per frame would serialize the shared
// command queue.

shared::Result<void> D3D12Renderer::UpdateTexturePixels(
    const uint8_t* pixels, int width, int height, int rowPitch) {

    if (!m_initialized) {
        return shared::Result<void>(shared::ErrorCode::NotInitialized,
                                    "Renderer not initialized");
    }
    if (!pixels || width <= 0 || height <= 0) {
        return shared::Result<void>(shared::ErrorCode::InvalidArgument,
                                    "Invalid pixel data");
    }

    // NOTE: no m_renderMutex lock here on purpose. The only caller
    // (Engine::RenderLoop) invokes this between BeginFrame() and
    // EndFrame(), which already hold the mutex for the whole frame.
    // Recursive locking on the render thread would throw
    // std::system_error (MSVC detects the re-lock) and abort the process.
    ID3D12Device* dev = m_device.Get();

    bool textureRecreated = false;
    if (!m_texture || m_texWidth != width || m_texHeight != height) {
        if (m_texture) {
            ActiveQueue().WaitForGPU();
            m_texture.Reset();
            m_textureUploadBuffer.Reset();
        }
        m_textureRecreations++;
        textureRecreated = true;

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width            = static_cast<UINT64>(width);
        texDesc.Height           = static_cast<UINT>(height);
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels        = 1;
        texDesc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        m_texture = m_device.CreateCommittedResource(
            heapProps, D3D12_HEAP_FLAG_NONE, texDesc, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!m_texture) {
            if (IsDeviceRemovedHr(m_device.GetDeviceRemovedReason())) m_deviceRemoved = true;
            return shared::Result<void>(shared::ErrorCode::OutOfMemory,
                                        "Failed to create video texture");
        }

        // Row pitch of a buffer -> texture copy must be aligned to
        // D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (256). Decoder row pitches
        // (e.g. 2706*4 = 10824) rarely are, so pad each row in the
        // upload buffer and use the aligned pitch in the footprint.
        const UINT64 alignedPitch =
            (static_cast<UINT64>(rowPitch) + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
            & ~(static_cast<UINT64>(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) - 1);

        UINT64 uploadSize = alignedPitch * height;
        uploadSize = (uploadSize + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1)
                   & ~(static_cast<UINT64>(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT) - 1);

        D3D12_RESOURCE_DESC uploadDesc = {};
        uploadDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        uploadDesc.Width            = uploadSize;
        uploadDesc.Height           = 1;
        uploadDesc.DepthOrArraySize = 1;
        uploadDesc.MipLevels        = 1;
        uploadDesc.Format           = DXGI_FORMAT_UNKNOWN;
        uploadDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        uploadDesc.SampleDesc.Count = 1;

        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        m_textureUploadBuffer = m_device.CreateCommittedResource(
            heapProps, D3D12_HEAP_FLAG_NONE, uploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!m_textureUploadBuffer) {
            if (IsDeviceRemovedHr(m_device.GetDeviceRemovedReason())) m_deviceRemoved = true;
            m_texture.Reset();
            return shared::Result<void>(shared::ErrorCode::OutOfMemory,
                                        "Failed to create video upload buffer");
        }

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels     = 1;

        D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
        dev->CreateShaderResourceView(m_texture.Get(), &srvDesc, cpu);
        m_textureSRVGpuHandle = m_srvHeap->GetGPUDescriptorHandleForHeapStart();

        m_texWidth  = width;
        m_texHeight = height;
        m_hasTexture = true;

        m_textureBytes = static_cast<uint64_t>(width) * height * 4;
        m_uploadBytes  = uploadSize;
    }

    // Copy pixels to upload buffer
    void* mapped = nullptr;
    HRESULT hr = m_textureUploadBuffer->Map(0, nullptr, &mapped);
    if (FAILED(hr)) {
        if (IsDeviceRemovedHr(hr)) m_deviceRemoved = true;
        return shared::Result<void>(shared::ErrorCode::UnknownError, "Map failed");
    }

    {
        const uint8_t* src = pixels;
        uint8_t* dst = static_cast<uint8_t*>(mapped);
        const UINT64 alignedPitch =
            (static_cast<UINT64>(rowPitch) + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
            & ~(static_cast<UINT64>(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) - 1);
        for (int row = 0; row < height; ++row) {
            memcpy(dst, src, static_cast<size_t>(rowPitch));
            src += rowPitch;
            dst += alignedPitch;
        }
    }
    m_textureUploadBuffer->Unmap(0, nullptr);

    // Persistent context ring (P5) — no per-upload allocation.
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> copyAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> copyList;
    if (auto ctxRes = AcquireCopyContext(copyAllocator, copyList); ctxRes.IsError()) {
        return ctxRes;
    }
    copyList->Reset(copyAllocator.Get(), nullptr);

    // A texture that already exists was left in PIXEL_SHADER_RESOURCE by the
    // previous upload (and by every frame that sampled it). Without this
    // transition the CopyTextureRegion below records a destination in the
    // wrong state — a debug-layer error and an undefined copy from the
    // second upload onwards. A freshly created texture is already COPY_DEST.
    if (!textureRecreated) {
        D3D12_RESOURCE_BARRIER toCopyDest = {};
        toCopyDest.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toCopyDest.Transition.pResource   = m_texture.Get();
        toCopyDest.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        toCopyDest.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        toCopyDest.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        copyList->ResourceBarrier(1, &toCopyDest);
    }

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource        = m_texture.Get();
    dstLoc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = m_textureUploadBuffer.Get();
    srcLoc.Type      = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint.Offset = 0;
    srcLoc.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
    srcLoc.PlacedFootprint.Footprint.Width    = static_cast<UINT>(width);
    srcLoc.PlacedFootprint.Footprint.Height   = static_cast<UINT>(height);
    srcLoc.PlacedFootprint.Footprint.Depth    = 1;
    srcLoc.PlacedFootprint.Footprint.RowPitch =
        static_cast<UINT>((static_cast<UINT64>(rowPitch)
            + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
            & ~(static_cast<UINT64>(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) - 1));

    copyList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = m_texture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    copyList->ResourceBarrier(1, &barrier);

    copyList->Close();
    ActiveQueue().Execute(copyList.Get());
    ActiveQueue().WaitForGPU();

    m_uploads++;

    return shared::Result<void>();
}

shared::Result<void> D3D12Renderer::UpdateTexturePixels(
    std::span<const uint8_t> pixels, int width, int height, int rowPitch) {
    if (pixels.size() < static_cast<size_t>(rowPitch) * height) {
        return shared::Result<void>(shared::ErrorCode::InvalidArgument,
                                    "pixel buffer too small for given dimensions");
    }
    return UpdateTexturePixels(pixels.data(), width, height, rowPitch);
}

// ─── Frame Rendering ──────────────────────────────────────────────────────

// P5: persistent copy-context ring. After warm-up this never allocates;
// the counter only moves if a context had to be (re)created during an
// upload — steady-state growth there is a regression signal.
shared::Result<void> D3D12Renderer::AcquireCopyContext(
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>& allocatorOut,
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>& listOut) {
    CopyContext& ctx = m_copyContexts[m_copyContextNext];
    m_copyContextNext = (m_copyContextNext + 1) % kCopyContextCount;

    if (!ctx.allocator || !ctx.list) {
        m_allocatorsPerUpload.fetch_add(1, std::memory_order_relaxed);
        ctx.allocator = m_device.CreateCommandAllocator();
        if (!ctx.allocator) {
            if (IsDeviceRemovedHr(m_device.GetDeviceRemovedReason())) {
                m_deviceRemoved = true;
            }
            return shared::Result<void>(shared::ErrorCode::UnknownError,
                                        "CreateCommandAllocator failed");
        }
        ctx.list = m_device.CreateCommandList(ctx.allocator.Get());
        if (!ctx.list) {
            if (IsDeviceRemovedHr(m_device.GetDeviceRemovedReason())) {
                m_deviceRemoved = true;
            }
            return shared::Result<void>(shared::ErrorCode::UnknownError,
                                        "CreateCommandList failed");
        }
        // A list is created OPEN; this Close() must succeed or every later
        // Reset() starts from an invalid state (audit 5.6 #13).
        const HRESULT ctxCloseHr = ctx.list->Close();
        if (FAILED(ctxCloseHr)) {
            if (IsDeviceRemovedHr(m_device.GetDeviceRemovedReason())) {
                m_deviceRemoved = true;
            }
            return shared::Result<void>(shared::ErrorCode::UnknownError,
                                        "command list Close failed");
        }
    }

    allocatorOut = ctx.allocator;
    listOut      = ctx.list;
    return shared::Result<void>();
}

bool D3D12Renderer::BeginFrame() {
    m_renderMutex.lock();
    if (!BeginFrameLocked()) {
        m_renderMutex.unlock();
        return false;
    }
    return true;
}

bool D3D12Renderer::BeginFrameLocked() {
    // R1: arm the reentry detector while the frame lock is held; EndFrame
    // (and the early-out below) disarm it before the caller unlocks.
    m_frameLock.EnterFrame();

    m_lastFrameLatencyWaitMs.store(m_swapChain.MeasureWaitForFrameMs(),
                                   std::memory_order_relaxed);

    // P0-4: reset only a context whose fence completed. With max frame
    // latency 2 the ring always has a ready context absent a real GPU
    // hitch; on a hitch wait bounded, then escalate to device-loss
    // recovery instead of recycling in-flight command memory.
    FrameContext& ctx = m_frameContexts[graphics::FrameContextIndex(
        m_frameNumber, graphics::kFrameContextCount)];
    if (!graphics::FrameContextReady(ctx.fenceValue,
                                      ActiveQueue().GetCompletedValue())) {
        constexpr DWORD kFrameFenceWaitMs = 2000;
        if (!ActiveQueue().WaitForValue(ctx.fenceValue, kFrameFenceWaitMs)) {
            REX_LOG_ERROR("D3D12", "Frame fence {} not reached in {} ms; "
                                   "flagging device removed",
                          ctx.fenceValue, kFrameFenceWaitMs);
            m_deviceRemoved = true;
            m_frameLock.ExitFrame();
            return false;
        }
    }
    ctx.allocator->Reset();

    // The context's fence has completed, so its peak-detection readback
    // (recorded 2-3 frames ago) is safe to read now.
    const UINT ctxIndex = graphics::FrameContextIndex(
        m_frameNumber, graphics::kFrameContextCount);
    ConsumeToneStats(ctxIndex);
    // Advance the smoothed source peak once per rendered frame, exactly
    // like libplacebo's IIR (20-frame smoothing period), and rebuild the
    // tone curve from it. The measurement itself only changes when the
    // decoded frame changes, but the smoothing runs per rendered frame -
    // which is what mpv does too.
    if (m_measuredMaxPq > 0.0f) {
        m_peakState.Update(m_measuredMaxPq, m_measuredAvgPq);
        m_toneCurve = tone::BuildSplineCurve(m_peakState.maxPq, m_peakState.avgPq);
        // Log the detected peak when it moves materially (2 % of the PQ range
        // is roughly a 6 % change in nits): the curve's input is the one
        // thing a screenshot cannot show, and a wrong peak is the difference
        // between "matches mpv" and "looks plausible" - but a moving scene
        // must not spam the log.
        if (std::fabs(m_peakState.maxPq - m_loggedPeakPq) > 0.02f) {
            m_loggedPeakPq = m_peakState.maxPq;
            REX_LOG_INFO("D3D12",
                         "tone peak: max={} nits (PQ {}) avg={} PQ "
                         "-> srcPivot={} dstPivot={} slope={} inMax={}",
                         tone::PqDecode(m_peakState.maxPq), m_peakState.maxPq,
                         m_peakState.avgPq, m_toneCurve.srcPivot,
                         m_toneCurve.dstPivot, m_toneCurve.slope, m_toneCurve.inMax);
        }
    }
    const HRESULT listResetHr =
        m_commandList->Reset(ctx.allocator.Get(), m_pipelineState.Get());
    // Audit 5.6 #13: unchecked Reset() let a dead command list look alive —
    // the frame would then submit garbage or fail at Close with no link to
    // the cause. Treat reset failure like the fence-wait failure above.
    if (FAILED(listResetHr)) {
        REX_LOG_ERROR("D3D12", "Command list Reset failed (0x{:08X})",
                      static_cast<unsigned>(listResetHr));
        if (IsDeviceRemovedHr(m_device.GetDeviceRemovedReason())) {
            m_deviceRemoved = true;
        }
        m_frameLock.ExitFrame();
        return false;
    }
    m_stateTracker.Reset();
    return true;
}
void D3D12Renderer::RenderFrame() {
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_swapChain.GetRTVHandle(
        m_swapChain.GetCurrentIndex());
    ID3D12Resource* backBuffer = m_swapChain.GetBuffer(
        m_swapChain.GetCurrentIndex());

    // Transition back buffer to render target
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = backBuffer;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &barrier);

    m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
    m_commandList->ClearRenderTargetView(rtvHandle, m_clearColor, 0, nullptr);

    // Viewport with fill mode
    D3D12_VIEWPORT viewport;
    D3D12_RECT scissor;
    ComputeFillViewport(viewport, scissor);
    m_commandList->RSSetViewports(1, &viewport);
    m_commandList->RSSetScissorRects(1, &scissor);

    // Draw textured fullscreen quad (video frame takes priority over the
    // static texture).
    ID3D12Resource* texToDraw = nullptr;
    D3D12_GPU_DESCRIPTOR_HANDLE srvToDraw{};
    bool videoActive = false;
    if (m_hasVideoFrame && m_activeVideoHandle) {
        for (UINT i = 0; i < m_videoSlotCount; ++i) {
            if (m_videoSlots[i].sharedHandle == m_activeVideoHandle &&
                m_videoSlots[i].poolGeneration == m_activePoolGeneration) {
                texToDraw = m_videoSlots[i].resource.Get();
                srvToDraw = m_videoSlots[i].srvGpu;
                videoActive = true;
                break;
            }
        }
    } else if (m_hasTexture) {
        texToDraw = m_texture.Get();
        srvToDraw = m_textureSRVGpuHandle;
    }

    if (texToDraw) {
        // Shared textures arrive in D3D12_RESOURCE_STATE_COMMON; transition
        // for sampling, then return to COMMON so the D3D11 video engine can
        // blt into the slot again.
        if (videoActive) {
            m_stateTracker.Transition(m_commandList.Get(), texToDraw,
                                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }

        // ── Peak detection (HDR -> SDR tone curve input) ──────────────
        // Measures the source peak / average in PQ space, the same way
        // libplacebo does before it builds its spline. Only needed when
        // the shader will actually tone map, and only re-measured when the
        // decoded frame changed.
        const bool useHdrPso = m_forceSdr ? false : m_isHdrOutput;
        const bool hdrInput =
            m_activeColorInfo.transfer != VideoColorInfo::Transfer::SDR;
        if (videoActive && hdrInput && !useHdrPso) {
            DispatchToneStats(
                graphics::FrameContextIndex(m_frameNumber, graphics::kFrameContextCount),
                srvToDraw);
        }

        m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());
        ID3D12PipelineState* pso = nullptr;
        // --force-sdr: always use SDR PSO (R8G8B8A8_UNORM) even if swap chain
        // is R16F, because our NV12 shader outputs sRGB, not PQ.
        if (m_customPsActive) {
            pso = useHdrPso ? m_customPSOHdr.Get() : m_customPSO.Get();
        }
        if (!pso) {
            pso = useHdrPso ? m_pipelineStateHdr.Get()
                            : m_pipelineState.Get();
        }
        if (!pso) {
            // Audit 2026-10-17, #4 residual: the flag says HDR but the HDR
            // PSO does not exist (its creation failed and the SDR revert
            // also failed). Never bind null — the draw would be dropped
            // with the whole pass. Draw with the SDR pipeline until the
            // next refresh retries the HDR PSO.
            if (!m_hdrPsoMissingWarned) {
                m_hdrPsoMissingWarned = true;
                REX_LOG_WARN("D3D12",
                             "HDR PSO missing while the HDR flag is set — "
                             "using the SDR pipeline until the next refresh");
            }
            pso = m_pipelineState.Get();
        }
        m_commandList->SetPipelineState(pso);
        m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

        D3D12_VERTEX_BUFFER_VIEW vbv = {};
        vbv.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress();
        vbv.SizeInBytes    = sizeof(Vertex) * 4;
        vbv.StrideInBytes  = sizeof(Vertex);
        m_commandList->IASetVertexBuffers(0, 1, &vbv);

        ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
        m_commandList->SetDescriptorHeaps(1, heaps);
        m_commandList->SetGraphicsRootDescriptorTable(0, srvToDraw);

        // Color constants: decode/transfer, adjustments, HDR metadata.
        m_isStaticTexture = m_hasTexture && !videoActive;
        float cb[16] = {};
        BuildColorConstants(cb, m_isHdrOutput);
        // 15 = the whole b0 payload. It cannot be 16: the root-signature
        // budget is already at 64/64 DWORDs (audit 5.6 #9 — see the
        // root-signature comment).
        m_commandList->SetGraphicsRoot32BitConstants(1, 15, cb, 0);

        // Transform matrix (rotation/flip) for the vertex shader.
        m_commandList->SetGraphicsRoot32BitConstants(2, 16,
                                                     &m_transformMatrix, 0);

        // Audio spectrum (b2, 32 floats) — zeroed when no audio capture.
        m_commandList->SetGraphicsRoot32BitConstants(3, 32,
                                                     &m_currentSpectrum, 0);

        // Crop driver padding out of the sampled region. Video pools may
        // be larger than the active picture (e.g. 1920x1152 surface for
        // 1920x1080 content); sampling raw 0..1 would present zeroed
        // padding rows as a green band and squeeze the picture.
        // Static textures always fit exactly.
        if (videoActive) {
            EnsureVertexUVs(m_videoUvScaleX, m_videoUvScaleY);
        } else {
            EnsureVertexUVs(1.0f, 1.0f);
        }

        m_commandList->DrawInstanced(4, 1, 0, 0);

        if (videoActive) {
            m_stateTracker.Transition(m_commandList.Get(), texToDraw,
                                      D3D12_RESOURCE_STATE_COMMON);
        }
    }

    // Transition back to present — via COPY_SOURCE when this frame is the
    // quit snapshot. The copy runs after the draw and before the buffer is
    // handed to the compositor, so the JPEG is exactly the picture that was
    // presented (fill mode, adjustments, rotation and tone map included).
    barrier.Transition.pResource   = backBuffer;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter  = m_captureReadback
                                         ? D3D12_RESOURCE_STATE_COPY_SOURCE
                                         : D3D12_RESOURCE_STATE_PRESENT;
    m_commandList->ResourceBarrier(1, &barrier);

    if (m_captureReadback) {
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource       = m_captureReadback.Get();
        dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = m_captureFootprint;

        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource        = backBuffer;
        src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;

        m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
        m_commandList->ResourceBarrier(1, &barrier);
    }
}

void D3D12Renderer::EndFrame() {
    EndFrameLocked();
    m_renderMutex.unlock();
}

bool D3D12Renderer::EndFrameLocked() {
    const HRESULT closeHr = m_commandList->Close();
    // Audit 5.6 #13: never ExecuteCommandLists a list that failed to Close —
    // dropping this frame is safer than submitting corrupted command memory.
    if (FAILED(closeHr)) {
        REX_LOG_ERROR("D3D12", "Command list Close failed (0x{:08X})",
                      static_cast<unsigned>(closeHr));
        if (IsDeviceRemovedHr(m_device.GetDeviceRemovedReason())) {
            m_deviceRemoved = true;
        }
        m_frameLock.ExitFrame();
        return false;
    }
    ActiveQueue().Execute(m_commandList.Get());

    HRESULT hr = m_swapChain.Present(1, 0);

    // Display cadence, straight from DXGI: how many refreshes the frame just
    // presented occupied, and when it synced. This is the only signal in REX
    // that describes what the panel showed rather than what this thread
    // submitted — see wall_research.md P10 for why the submit-side gap metric
    // alone is not a smoothness verdict.
    {
        DisplayCadenceStats::Sample s{};
        if (m_swapChain.SampleDisplayCadence(s)) {
            m_lastDwell.store(static_cast<double>(s.presentRefreshCount),
                              std::memory_order_relaxed);
        }
    }

    // Feed the per-swapchain occlusion tracker BEFORE branching: S_OK
    // clears the run, OCCLUDED extends it, real failures end it (the
    // device path — not parking — owns failures).
    m_occlusion.NotifyPresent(static_cast<long>(hr));

    // ── Present diagnostics ──────────────────────────────────────────────
    //
    // DXGI_STATUS_OCCLUDED (0x087A0001) is a SUCCESS code returned when
    // DWM drops the frame because it considers the window fully occluded.
    // FAILED() and hr < 0 checks will NOT catch it — it is semantically
    // "success but no-op."

    if (FAILED(hr)) {
        REX_LOG_ERROR("D3D12", "Present FAILED (0x{:08x})",
                     static_cast<unsigned long>(hr));

        if (hr == DXGI_ERROR_DEVICE_REMOVED) {
            HRESULT drr = m_device.GetDeviceRemovedReason();
            REX_LOG_ERROR("D3D12", "Device removed, reason 0x{:08x}",
                         static_cast<unsigned long>(drr));
            m_deviceRemoved = true;
        } else if (hr == DXGI_ERROR_DEVICE_RESET) {
            REX_LOG_WARN("D3D12", "Device reset — will attempt recovery");
            m_deviceRemoved = true;
        } else if (IsDeviceRemovedHr(hr)) {
            m_deviceRemoved = true;
        }
    } else if (hr == DXGI_STATUS_OCCLUDED) {
        // DWM dropped this frame. Throttle to ~1 log every 10 seconds
        // so the log isn't spammed at 60 fps. The throttle rides the
        // occlusion tracker's consecutive count (single source of truth).
        if (m_occlusion.ConsecutiveOccluded() % 300 == 1) {
            REX_LOG_WARN("D3D12",
                "Present returned DXGI_STATUS_OCCLUDED. "
                "DWM considers the render target fully occluded "
                "(frame count: {}).", m_occlusion.ConsecutiveOccluded());
        }
    } else if (hr != S_OK) {
        REX_LOG_INFO("D3D12", "Present returned non-S_OK: 0x{:08x}",
                    static_cast<unsigned long>(hr));
    } else {
        static thread_local int s_presentCount = 0;
        s_presentCount++;
        if (s_presentCount == 1 || s_presentCount % 600 == 0) {
            REX_LOG_INFO("D3D12", "Present OK (frame count: {})", s_presentCount);
        }
    }

    // P0-4: tag the context whose allocator backs this frame's list, so
    // the next wrap-around waits for it instead of resetting live memory.
    m_frameContexts[graphics::FrameContextIndex(
        m_frameNumber, graphics::kFrameContextCount)].fenceValue =
            ActiveQueue().Signal();
    ++m_frameNumber;
    m_frameLock.ExitFrame();
    return true;
}

// ─── Frame snapshot ───────────────────────────────────────────────────────

namespace {
std::wstring Utf8ToWide(std::string_view sv) {
    if (sv.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, sv.data(),
                                        static_cast<int>(sv.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()),
                        out.data(), len);
    return out;
}
} // namespace

bool D3D12Renderer::CaptureFrameToJpeg(std::string_view pathUtf8) {
    const std::wstring path = Utf8ToWide(pathUtf8);
    if (path.empty() || !m_initialized) return false;

    // Hold the frame mutex across the setup AND the capture frame: the HDR
    // switch below calls ResizeBuffers, which must never race a frame that
    // is still being recorded into a back buffer.
    std::lock_guard<std::mutex> lock(m_renderMutex);
    if (m_deviceRemoved) return false;
    if (!m_hasVideoFrame && !m_hasTexture) {
        // Nothing is loaded (e.g. an image wallpaper already owns the
        // desktop). A black clear would be a lie, so leave the desktop alone.
        REX_LOG_INFO("D3D12", "frame snapshot skipped: no scene on screen");
        return false;
    }

    const UINT width  = static_cast<UINT>(m_width);
    const UINT height = static_cast<UINT>(m_height);
    if (width == 0 || height == 0) return false;

    // HDR presents FP16/PQ, which an 8-bit JPEG cannot carry. Render the
    // capture frame through the SDR pipeline instead: the shader's tone map (the
    // mpv/libplacebo-parity curve) produces the SDR rendition of the very
    // same picture. Deliberately not restored — this runs on the way out.
    if (m_swapChain.IsHDRActive()) {
        REX_LOG_INFO("D3D12", "frame snapshot: rendering the capture frame in SDR");
        m_swapChain.SetForceSdr(true);
        SyncHdrStateLocked();
    }

    // D3D12 forbids textures on READBACK heaps, so the capture target is a
    // readback BUFFER sized by the copy footprints. CopyTextureRegion then
    // fills it row by row; RowPitch is 256-byte aligned and is NOT width*4.
    ID3D12Resource* backBuffer =
        m_swapChain.GetBuffer(m_swapChain.GetCurrentIndex());
    if (!backBuffer) {
        REX_LOG_ERROR("D3D12", "frame snapshot: no back buffer to capture");
        return false;
    }
    const D3D12_RESOURCE_DESC backDesc = backBuffer->GetDesc();

    // The narrowing below understands exactly the two SDR formats the swap
    // chain can use. Anything else (in practice FP16, i.e. the HDR switch
    // above did not take) would be decoded as if it were 8-bit RGBA and
    // produce a garbage image — and this image becomes the user's wallpaper,
    // so refuse instead of writing it.
    if (backDesc.Format != DXGI_FORMAT_R10G10B10A2_UNORM &&
        backDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
        REX_LOG_ERROR("D3D12", "frame snapshot: unsupported back-buffer format {}",
                      static_cast<int>(backDesc.Format));
        return false;
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 totalBytes = 0;
    m_device.Get()->GetCopyableFootprints(&backDesc, 0, 1, 0, &footprint,
                                          nullptr, nullptr, &totalBytes);
    if (totalBytes == 0) {
        REX_LOG_ERROR("D3D12", "frame snapshot: copy footprint is empty");
        return false;
    }

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width            = totalBytes;
    bufferDesc.Height           = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels        = 1;
    bufferDesc.Format           = DXGI_FORMAT_UNKNOWN;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type             = D3D12_HEAP_TYPE_READBACK;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask  = 1;

    HRESULT hr = m_device.Get()->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&m_captureReadback));
    if (FAILED(hr)) {
        REX_LOG_ERROR("D3D12", "frame snapshot: readback buffer allocation failed "
                               "(0x{:08X}, {} bytes, row pitch {})",
                      static_cast<unsigned>(hr),
                      static_cast<unsigned long long>(totalBytes),
                      footprint.Footprint.RowPitch);
        return false;
    }
    m_captureFootprint = footprint;

    if (!BeginFrameLocked()) {
        m_captureReadback.Reset();
        return false;
    }
    RenderFrame();   // copies the finished frame into m_captureReadback
    const bool submitted = EndFrameLocked();
    if (!submitted) {
        m_captureReadback.Reset();
        return false;
    }

    // Wait for THIS frame's copy only — the render loop may keep submitting.
    const UINT64 fence = ActiveQueue().Signal();
    if (!ActiveQueue().WaitForValue(fence, 10000)) {
        REX_LOG_ERROR("D3D12", "frame snapshot: GPU readback timed out");
        m_captureReadback.Reset();
        return false;
    }

    D3D12_RANGE readRange{0, static_cast<SIZE_T>(totalBytes)};
    void* mapped = nullptr;
    hr = m_captureReadback->Map(0, &readRange, &mapped);
    if (FAILED(hr) || !mapped) {
        REX_LOG_ERROR("D3D12", "frame snapshot: readback map failed "
                               "(0x{:08X})", static_cast<unsigned>(hr));
        m_captureReadback.Reset();
        return false;
    }

    // Back buffer -> 8-bit BGRA, the encoder's source layout. The SDR pipeline
    // writes transfer-encoded sRGB values into a plain UNORM target, so a
    // straight 10/8-bit narrowing is the correct conversion (no second
    // transfer function).
    const DXGI_FORMAT format = backDesc.Format;
    const UINT rowPitch = m_captureFootprint.Footprint.RowPitch;
    std::vector<uint8_t> bgra(static_cast<size_t>(width) * height * 4);
    const uint8_t* srcBase = static_cast<const uint8_t*>(mapped);
    for (UINT y = 0; y < height; ++y) {
        const uint32_t* row = reinterpret_cast<const uint32_t*>(
            srcBase + static_cast<size_t>(y) * rowPitch);
        uint8_t* dst = bgra.data() + static_cast<size_t>(y) * width * 4;
        for (UINT x = 0; x < width; ++x) {
            const uint32_t p = row[x];
            uint8_t r, g, b;
            if (format == DXGI_FORMAT_R10G10B10A2_UNORM) {
                r = static_cast<uint8_t>(((p & 0x3FF) * 255u + 511u) / 1023u);
                g = static_cast<uint8_t>((((p >> 10) & 0x3FF) * 255u + 511u) / 1023u);
                b = static_cast<uint8_t>((((p >> 20) & 0x3FF) * 255u + 511u) / 1023u);
            } else {
                // R8G8B8A8_UNORM (the driver refused a 10-bit SDR buffer).
                r = static_cast<uint8_t>(p & 0xFF);
                g = static_cast<uint8_t>((p >> 8) & 0xFF);
                b = static_cast<uint8_t>((p >> 16) & 0xFF);
            }
            dst[0] = b;
            dst[1] = g;
            dst[2] = r;
            dst[3] = 255;
            dst += 4;
        }
    }
    m_captureReadback->Unmap(0, nullptr);
    m_captureReadback.Reset();

    if (!WriteBgraJpeg(path, bgra.data(), width, height, width * 4)) {
        return false;
    }
    REX_LOG_INFO("D3D12", "frame snapshot written: {}x{} -> {}",
                 width, height, pathUtf8);
    return true;
}

bool D3D12Renderer::WriteBgraJpeg(const std::wstring& path, const uint8_t* bgra,
                                  UINT width, UINT height, UINT rowPitch) {
    if (!bgra || width == 0 || height == 0 || rowPitch < width * 4) return false;

    // WIC needs COM on this thread. RPC_E_CHANGED_MODE means COM is already
    // initialized with another model, which is fine — we just must not
    // uninitialize in that case.
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool ownsCom = SUCCEEDED(coHr);

    // Encode to a sibling temp file and rename on success. The desktop is
    // pointed at `path` right after this returns, and Windows re-reads that
    // path at sign-in — so a process kill or crash halfway through the encode
    // must never leave a truncated JPEG where the wallpaper loader will find
    // it. MOVEFILE_WRITE_THROUGH flushes the rename before we report success.
    const std::wstring tempPath = path + L".tmp";
    DeleteFileW(tempPath.c_str());

    bool ok = false;
    {
        Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
        Microsoft::WRL::ComPtr<IWICStream> stream;
        Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
        Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
        Microsoft::WRL::ComPtr<IPropertyBag2> props;

        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                      CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&factory));
        if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
        if (SUCCEEDED(hr))
            hr = stream->InitializeFromFilename(tempPath.c_str(), GENERIC_WRITE);
        if (SUCCEEDED(hr))
            hr = factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder);
        if (SUCCEEDED(hr))
            hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, &props);
        if (SUCCEEDED(hr) && props) {
            // q95 keeps the still visually indistinguishable from the frame.
            // Measured on a 3840x2160 frame: 23 ms, versus 710 ms for the PNG
            // this used to write - the quit still is on the desktop ~0.3 s
            // after SHUTDOWN instead of ~1 s.
            PROPBAG2 option = {};
            option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
            VARIANT quality;
            VariantInit(&quality);
            quality.vt = VT_R4;
            quality.fltVal = 0.95f;
            hr = props->Write(1, &option, &quality);
            VariantClear(&quality);
        }
        if (SUCCEEDED(hr)) hr = frame->Initialize(props.Get());
        if (SUCCEEDED(hr)) hr = frame->SetSize(width, height);

        // JPEG has no alpha channel; the encoder asks for 24bppBGR and the
        // WriteSource path below lets WIC convert the BGRA capture.
        WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
        if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&fmt);
        if (SUCCEEDED(hr)) {
            const UINT bufferSize = rowPitch * height;
            if (IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA)) {
                hr = frame->WritePixels(height, rowPitch, bufferSize,
                                        const_cast<BYTE*>(bgra));
            } else {
                // The encoder picked another layout; hand it the pixels as a
                // source bitmap and let WIC convert.
                Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
                hr = factory->CreateBitmapFromMemory(
                    width, height, GUID_WICPixelFormat32bppBGRA, rowPitch,
                    bufferSize, const_cast<BYTE*>(bgra), &bitmap);
                if (SUCCEEDED(hr)) hr = frame->WriteSource(bitmap.Get(), nullptr);
            }
        }
        if (SUCCEEDED(hr)) hr = frame->Commit();
        if (SUCCEEDED(hr)) hr = encoder->Commit();

        ok = SUCCEEDED(hr);
        if (!ok) {
            REX_LOG_ERROR("D3D12", "frame snapshot: JPEG encode failed "
                                   "(0x{:08X})", static_cast<unsigned>(hr));
        }
    }

    if (ownsCom) CoUninitialize();

    if (ok) {
        if (!MoveFileExW(tempPath.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            REX_LOG_ERROR("D3D12", "frame snapshot: JPEG rename failed ({})",
                          GetLastError());
            ok = false;
        }
    }
    if (!ok) DeleteFileW(tempPath.c_str());
    return ok;
}

void D3D12Renderer::SetClearColor(float r, float g, float b) {
    m_clearColor[0] = r;
    m_clearColor[1] = g;
    m_clearColor[2] = b;
    m_clearColor[3] = 1.0f;
}

void D3D12Renderer::Resize(int width, int height) {
    if (width == m_width && height == m_height) return;

    std::lock_guard<std::mutex> lock(m_renderMutex);

    REX_LOG_INFO("D3D12", "Resizing to {}x{}", width, height);

    ActiveQueue().WaitForGPU();
    m_swapChain.Resize(m_device.Get(), width, height);
    // The window may have moved outputs (or the OS HDR toggle flipped):
    // the swapchain re-evaluated its selection above — follow it.
    SyncHdrStateLocked();

    m_width  = width;
    m_height = height;
}

// ─── HDR output refresh (P0-2 / R16) ─────────────────────────────────────
// m_isHdrOutput was snapshot-once at Initialize and the HDR PSO existed
// only when starting HDR: after an OS toggle the renderer bound the wrong
// PSO (SDR PSO on an R16F/PQ swapchain or vice versa).

void D3D12Renderer::RefreshHdrOutput() {
    std::lock_guard<std::mutex> lock(m_renderMutex);
    SyncHdrStateLocked();
}

void D3D12Renderer::SyncHdrStateLocked() {
    if (!m_initialized) return;
    // R1: a format switch inside the refresh calls ResizeBuffers, which
    // requires all back-buffer references released and no in-flight work
    // sampling them — drain the queue first (cheap when already idle).
    ActiveQueue().WaitForGPU();
    auto sr = m_swapChain.RefreshHdrSelection(m_device.Get());
    if (sr.IsError()) {
        REX_LOG_WARN("D3D12", "HDR refresh failed: {}; keeping {} output",
                     sr.Error().message.c_str(),
                     m_isHdrOutput ? "HDR" : "SDR");
        return;
    }
    const auto action = graphics::PlanHdrSync(
        m_isHdrOutput, m_swapChain.IsHDR(),
        m_pipelineStateHdr.Get() != nullptr);
    // The SDR back-buffer format can change under us (the swapchain falls
    // back from 10-bit to 8-bit UNORM when the driver refuses the 10-bit
    // buffer). A PSO's RTV format must match the buffer it renders into,
    // so rebuild the SDR pipeline state when the format moved.
    if (!m_swapChain.IsHDR()) {
        const DXGI_FORMAT liveFormat = m_swapChain.GetBackBufferFormat();
        if (liveFormat != m_sdrPsoFormat) {
            REX_LOG_INFO("D3D12", "SDR back-buffer format changed to {}; "
                                   "rebuilding the pipeline state",
                         liveFormat == DXGI_FORMAT_R10G10B10A2_UNORM
                             ? "R10G10B10A2_UNORM" : "R8G8B8A8_UNORM");
            if (CreatePipelineState(liveFormat, m_pipelineState)) {
                m_sdrPsoFormat = liveFormat;
            } else {
                REX_LOG_ERROR("D3D12", "SDR pipeline state rebuild failed");
            }
        }
    }
    if (action.updateFlag) {
        m_isHdrOutput = m_swapChain.IsHDR();
        REX_LOG_INFO("D3D12", "HDR output {} (swapchain {})",
                     m_isHdrOutput ? "enabled" : "disabled",
                     m_isHdrOutput ? "R16F/PQ" : "UNORM/sRGB");
        if (m_isHdrOutput) {
            // The OS knows how bright "SDR white" should be on an HDR
            // display - it is the Windows "SDR content brightness" slider -
            // and exposes it as a fixed-point multiplier of the 80-nit
            // reference. Take it from there so SDR wallpapers on an HDR
            // panel follow the user's brightness choice instead of the
            // hard-coded 100 nits. On an SDR display the API has nothing to
            // report and the fallback stays.
            const float nits = graphics::QuerySdrWhiteLevelNits(
                m_swapChain.GetOutputDeviceName());
            if (nits > 0.0f && nits != m_sdrWhiteNits) {
                REX_LOG_INFO("D3D12", "SDR white level from the OS: {} nits",
                             nits);
                m_sdrWhiteNits = nits;
            }
        }
    }
    if (action.createHdrPso) {
        if (!CreatePipelineState(DXGI_FORMAT_R16G16B16A16_FLOAT,
                                 m_pipelineStateHdr)) {
            // Audit 2026-10-17, #4: this branch used to clear the cached
            // flag immediately. With the OS already on HDR that made the
            // pass bind the SDR PSO and sRGB math onto an R16F/PQ back
            // buffer (format mismatch + wrong transform). Bring flag and
            // buffers back into agreement instead: force the swapchain to
            // SDR and only claim SDR once that actually took. The forced
            // flag stays pinned so a later refresh cannot flip the format
            // back to HDR and fail again (each oscillation would cost two
            // ResizeBuffers + recreated RTVs).
            REX_LOG_ERROR("D3D12",
                          "HDR PSO creation failed; reverting output to SDR");
            m_swapChain.SetForceSdr(true);
            auto revert = m_swapChain.RefreshHdrSelection(m_device.Get());
            if (revert.IsError()) {
                REX_LOG_WARN("D3D12", "SDR revert refresh failed: {}",
                             revert.Error().message.c_str());
            }
            m_isHdrOutput = graphics::HdrFlagAfterPsoFailure(m_swapChain.IsHDR());
            if (m_isHdrOutput) {
                REX_LOG_WARN("D3D12",
                             "SDR revert did not take — still on R16F/PQ; "
                             "keeping the HDR flag so the PSO choice matches "
                             "the buffers (next refresh retries the PSO)");
            } else {
                REX_LOG_WARN("D3D12",
                             "staying on SDR output (HDR retry needs an app "
                             "restart)");
            }
        }
    }
}

// ─── Upload Texture to GPU (static) ───────────────────────────────────────

bool D3D12Renderer::UploadTextureToGPU(const TextureData& data) {
    // Destination texture in DEFAULT heap
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width            = static_cast<UINT64>(data.width);
    texDesc.Height           = static_cast<UINT>(data.height);
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels        = 1;
    texDesc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    m_texture = m_device.CreateCommittedResource(
        heapProps, D3D12_HEAP_FLAG_NONE, texDesc, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!m_texture) {
        REX_LOG_ERROR("D3D12", "CreateCommittedResource (texture) failed");
        if (IsDeviceRemovedHr(m_device.GetDeviceRemovedReason())) m_deviceRemoved = true;
        return false;
    }

    // R3: the decoder's rowPitch is tightly packed (width × 4); D3D12 placed
    // footprints require a 256-byte row pitch, so widths that aren't a
    // multiple of 64 pixels (1366/3440) corrupted or failed the copy. Pad
    // rows in the upload buffer and declare the padded pitch.
    constexpr UINT kRowPitchAlign = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT; // 256
    const UINT srcRowPitch = static_cast<UINT>(data.rowPitch);
    const UINT rowPitch =
        (srcRowPitch + (kRowPitchAlign - 1)) & ~(kRowPitchAlign - 1);

    // Upload buffer
    UINT64 uploadSize = static_cast<UINT64>(rowPitch) * data.height;
    uploadSize = (uploadSize + 511) & ~511ULL;

    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC uploadDesc = {};
    uploadDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width            = uploadSize;
    uploadDesc.Height           = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels        = 1;
    uploadDesc.Format           = DXGI_FORMAT_UNKNOWN;
    uploadDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    uploadDesc.SampleDesc.Count = 1;

    auto uploadBuffer = m_device.CreateCommittedResource(
        heapProps, D3D12_HEAP_FLAG_NONE, uploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!uploadBuffer) {
        REX_LOG_ERROR("D3D12", "CreateCommittedResource (upload) failed");
        if (IsDeviceRemovedHr(m_device.GetDeviceRemovedReason())) m_deviceRemoved = true;
        m_texture.Reset();
        return false;
    }

    // Copy pixel data
    void* mapped = nullptr;
    HRESULT hr = uploadBuffer->Map(0, nullptr, &mapped);
    if (FAILED(hr)) {
        if (IsDeviceRemovedHr(hr)) m_deviceRemoved = true;
        m_texture.Reset();
        return false;
    }
    {
        const uint8_t* src = data.pixels.data();
        uint8_t* dst = static_cast<uint8_t*>(mapped);
        for (int row = 0; row < data.height; ++row) {
            memcpy(dst, src, static_cast<size_t>(srcRowPitch));
            src += srcRowPitch;
            dst += rowPitch;
        }
    }
    uploadBuffer->Unmap(0, nullptr);

    // Copy via persistent context ring (P5) — no per-upload allocation.
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> uploadAlloc;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> uploadList;
    if (auto ctxRes = AcquireCopyContext(uploadAlloc, uploadList); ctxRes.IsError()) {
        m_texture.Reset();
        return false;
    }
    uploadList->Reset(uploadAlloc.Get(), nullptr);

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource        = m_texture.Get();
    dstLoc.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = uploadBuffer.Get();
    srcLoc.Type      = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint.Offset = 0;
    srcLoc.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
    srcLoc.PlacedFootprint.Footprint.Width    = static_cast<UINT>(data.width);
    srcLoc.PlacedFootprint.Footprint.Height   = static_cast<UINT>(data.height);
    srcLoc.PlacedFootprint.Footprint.Depth    = 1;
    srcLoc.PlacedFootprint.Footprint.RowPitch = rowPitch;

    uploadList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = m_texture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    uploadList->ResourceBarrier(1, &barrier);

    uploadList->Close();
    ActiveQueue().Execute(uploadList.Get());
    ActiveQueue().WaitForGPU();

    // Create SRV
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels     = 1;

    D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    m_device.Get()->CreateShaderResourceView(m_texture.Get(), &srvDesc, cpu);
    m_textureSRVGpuHandle = m_srvHeap->GetGPUDescriptorHandleForHeapStart();

    m_hasTexture = true;
    m_texWidth   = data.width;
    m_texHeight  = data.height;

    return true;
}

// ─── Pipeline State ──────────────────────────────────────────────────────

// ─── Transform (rotation / flip) ──────────────────────────────────────────

void D3D12Renderer::SetRotation(float degrees) {
    std::lock_guard<std::mutex> lock(m_renderMutex);
    m_rotation = degrees;
    UpdateTransformMatrix();
}

void D3D12Renderer::SetFlip(bool horizontal, bool vertical) {
    std::lock_guard<std::mutex> lock(m_renderMutex);
    m_flipH = horizontal;
    m_flipV = vertical;
    UpdateTransformMatrix();
}

void D3D12Renderer::UpdateTransformMatrix() {
    using namespace DirectX;
    XMMATRIX rot = XMMatrixRotationZ(XMConvertToRadians(m_rotation));
    XMMATRIX flip = XMMatrixScaling(m_flipH ? -1.0f : 1.0f,
                                     m_flipV ? -1.0f : 1.0f, 1.0f);
    // Row-major: the vertex shader uses row-vector mul(v, M), so the
    // stored matrix is consumed without a transpose.
    XMStoreFloat4x4(&m_transformMatrix, flip * rot);
}

// ─── Custom pixel shader hook ─────────────────────────────────────────────

bool D3D12Renderer::SetCustomPixelShader(const std::string& hlslSource) {
    if (!m_initialized) {
        REX_LOG_ERROR("D3D12", "Custom PS: renderer not initialized");
        return false;
    }

    std::lock_guard<std::mutex> lock(m_renderMutex);

    Microsoft::WRL::ComPtr<ID3DBlob> psBlob, errorBlob;
    HRESULT hr = D3DCompile(hlslSource.c_str(), hlslSource.size(), nullptr,
                            nullptr, nullptr, "main", "ps_5_0",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                            &psBlob, &errorBlob);
    if (FAILED(hr)) {
        std::string msg = errorBlob
            ? std::string(static_cast<const char*>(errorBlob->GetBufferPointer()),
                          errorBlob->GetBufferSize())
            : "unknown compile error";
        REX_LOG_ERROR("D3D12", "Custom PS compile failed: {}", msg);
        return false;
    }

    // Mirror the default pipeline setup: SDR always, HDR when the swap
    // chain is an R16F target. The root signature is shared, so the custom
    // shader must bind the same t0 texture / s0 sampler / b0 constants.
    Microsoft::WRL::ComPtr<ID3D12PipelineState> customSdr, customHdr;
    if (!CreatePipelineStateWithPs(DXGI_FORMAT_R8G8B8A8_UNORM,
                                   psBlob->GetBufferPointer(),
                                   psBlob->GetBufferSize(), customSdr)) {
        REX_LOG_ERROR("D3D12", "Custom PS: SDR PSO creation failed");
        return false;
    }
    if (m_isHdrOutput) {
        if (!CreatePipelineStateWithPs(DXGI_FORMAT_R16G16B16A16_FLOAT,
                                       psBlob->GetBufferPointer(),
                                       psBlob->GetBufferSize(), customHdr)) {
            REX_LOG_ERROR("D3D12", "Custom PS: HDR PSO creation failed");
            return false;
        }
    }

    m_customPSO = customSdr;
    m_customPSOHdr = customHdr;
    m_customPsSource = hlslSource;
    m_customPsActive = true;
    REX_LOG_INFO("D3D12", "Custom pixel shader installed");
    return true;
}

void D3D12Renderer::ClearCustomPixelShader() {
    std::lock_guard<std::mutex> lock(m_renderMutex);
    m_customPSO.Reset();
    m_customPSOHdr.Reset();
    m_customPsSource.clear();
    m_customPsActive = false;
    REX_LOG_INFO("D3D12", "Custom pixel shader reverted to default");
}

bool D3D12Renderer::CreateRootSignature() {
    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType             = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors        = 2;   // t0=Y plane, t1=UV plane (NV12)
    range.BaseShaderRegister    = 0;
    range.RegisterSpace         = 0;

    D3D12_ROOT_PARAMETER params[4] = {};

    // t0: texture (static or video slot)
    params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges   = &range;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // b0: color constants (mode / adjustments / display metadata) — 15
    // DWORDs, the most this signature can carry: descriptor table (1) +
    // b0 (15) + transform (16) + spectrum (32) = the 64-DWORD root-signature
    // limit exactly. Audit 5.6 #9: the old layout parked SDR white nits in an
    // unreachable 16th DWORD; it now shares the third lane of `extra` (see
    // the shader cbuffer), which IS shipped.
    params[1].ParameterType                    = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister         = 0;
    params[1].Constants.RegisterSpace          = 0;
    params[1].Constants.Num32BitValues         = 15;
    params[1].ShaderVisibility                 = D3D12_SHADER_VISIBILITY_PIXEL;

    // b1: transform matrix (rotation/flip) — 16 floats, vertex shader.
    params[2].ParameterType                    = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants.ShaderRegister         = 1;
    params[2].Constants.RegisterSpace          = 0;
    params[2].Constants.Num32BitValues         = 16;
    params[2].ShaderVisibility                 = D3D12_SHADER_VISIBILITY_VERTEX;

    // b2: audio spectrum — 32 floats (8 float4s), pixel shader.
    params[3].ParameterType                    = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[3].Constants.ShaderRegister         = 2;
    params[3].Constants.RegisterSpace          = 0;
    params[3].Constants.Num32BitValues         = 32;
    params[3].ShaderVisibility                 = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter         = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD         = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters     = 4;
    desc.pParameters       = params;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers   = &sampler;
    desc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    Microsoft::WRL::ComPtr<ID3DBlob> sigBlob, errorBlob;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                              &sigBlob, &errorBlob);
    if (FAILED(hr)) {
        REX_LOG_ERROR("D3D12", "SerializeRootSignature failed: {:08x}", hr);
        return false;
    }

    m_rootSignature = m_device.CreateRootSignature(
        sigBlob->GetBufferPointer(), sigBlob->GetBufferSize());
    if (!m_rootSignature) return false;

    return true;
}

bool D3D12Renderer::CreatePipelineState(
    DXGI_FORMAT rtvFormat,
    Microsoft::WRL::ComPtr<ID3D12PipelineState>& outPs) {
    Microsoft::WRL::ComPtr<ID3DBlob> psBlob, errorBlob;

    // The dither amplitude is one code of the OUTPUT depth, so it is
    // compiled into the shader per target format: 10-bit for the mpv-parity
    // R10G10B10A2 swapchain, 8-bit for the fallback, none for the FP16 HDR
    // output (which is not quantised). The rule lives in OutputDither.hpp
    // so the shader and the tests cannot drift apart.
    const int targetBits = graphics::OutputDitherBits(rtvFormat);
    // std::string(...) on the first half: two const char* would be pointer
    // arithmetic, not concatenation.
    const std::string source =
        "#define TARGET_BITS " + std::to_string(targetBits) + "\n" +
        std::string(g_pixelShaderSourceA) + g_pixelShaderSourceB;

    HRESULT hr = D3DCompile(source.c_str(), source.size(),
           nullptr, nullptr, nullptr, "main", "ps_5_0",
           D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psBlob, &errorBlob);
    if (FAILED(hr)) {
        // Print the HLSL diagnostics: "the shader did not compile" without
        // the compiler's message is unactionable.
        if (errorBlob && errorBlob->GetBufferSize() > 0) {
            const std::string errors(
                static_cast<const char*>(errorBlob->GetBufferPointer()),
                errorBlob->GetBufferSize());
            REX_LOG_ERROR("D3D12", "Pixel shader compile failed:\n{}",
                          errors.c_str());
        } else {
            REX_LOG_ERROR("D3D12", "Pixel shader compile failed (0x{:08X})",
                          static_cast<unsigned>(hr));
        }
        return false;
    }

    return CreatePipelineStateWithPs(rtvFormat,
                                     psBlob->GetBufferPointer(),
                                     psBlob->GetBufferSize(), outPs);
}

bool D3D12Renderer::CreatePipelineStateWithPs(
    DXGI_FORMAT rtvFormat,
    const void* psBytecode, SIZE_T psSize,
    Microsoft::WRL::ComPtr<ID3D12PipelineState>& outPs) {
    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, errorBlob;

    HRESULT hr = D3DCompile(g_vertexShaderSource, strlen(g_vertexShaderSource),
                nullptr, nullptr, nullptr, "main", "vs_5_0",
                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsBlob, &errorBlob);
    if (FAILED(hr)) {
        REX_LOG_ERROR("D3D12", "Vertex shader compile failed");
        return false;
    }

    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature            = m_rootSignature.Get();
    psoDesc.VS                        = {vsBlob->GetBufferPointer(), vsBlob->GetBufferSize()};
    psoDesc.PS                        = {psBytecode, psSize};
    psoDesc.InputLayout               = {inputLayout, 2};
    psoDesc.RasterizerState.FillMode  = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode  = D3D12_CULL_MODE_NONE;
    psoDesc.BlendState.RenderTarget[0].BlendEnable       = FALSE;
    psoDesc.BlendState.RenderTarget[0].SrcBlend          = D3D12_BLEND_ONE;
    psoDesc.BlendState.RenderTarget[0].DestBlend         = D3D12_BLEND_ZERO;
    psoDesc.BlendState.RenderTarget[0].BlendOp           = D3D12_BLEND_OP_ADD;
    psoDesc.BlendState.RenderTarget[0].SrcBlendAlpha     = D3D12_BLEND_ONE;
    psoDesc.BlendState.RenderTarget[0].DestBlendAlpha    = D3D12_BLEND_ZERO;
    psoDesc.BlendState.RenderTarget[0].BlendOpAlpha      = D3D12_BLEND_OP_ADD;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.SampleMask              = UINT_MAX;
    psoDesc.PrimitiveTopologyType   = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets        = 1;
    psoDesc.RTVFormats[0]           = rtvFormat;
    psoDesc.SampleDesc.Count        = 1;

    outPs = m_device.CreateGraphicsPipelineState(psoDesc);
    if (!outPs) {
        REX_LOG_ERROR("D3D12", "CreateGraphicsPipelineState failed (format {:04x})",
                      static_cast<unsigned int>(rtvFormat));
        return false;
    }

    return true;
}

// ─── Color Pipeline ──────────────────────────────────────────────────────

void D3D12Renderer::BuildColorConstants(float cb[16], bool hdrOutput) const {
    const VideoColorInfo& ci = m_activeColorInfo;

    // mode: matrix | transfer | fullRange | isHdrOutput
    cb[0] = static_cast<float>(ci.yuvMatrix);
    cb[1] = static_cast<float>(ci.transfer);
    cb[2] = ci.fullRange ? 1.0f : 0.0f;
    // --force-sdr overrides hdrOutput to 0 (SDR path in shader)
    cb[3] = (m_forceSdr || !hdrOutput) ? 0.0f : 1.0f;

    // adj: brightness | contrast | saturation | paper white
    cb[4] = m_brightness;                 // brightness (identity 1.0)
    cb[5] = m_contrast;                   // contrast (identity 1.0)
    cb[6] = m_saturation;                 // saturation (identity 1.0)
    cb[7] = m_paperWhiteNits;               // SDR reference white on HDR (nits)

    // disp: display peak | mastering max | mastering min | MaxCLL
    cb[8] = m_displayPeakNits;           // tone-mapping target (nits)
    cb[9] = ci.masteringDisplayMaxLuminance > 0.0f
                ? ci.masteringDisplayMaxLuminance : 1000.0f;
    cb[10] = ci.masteringDisplayMinLuminance;
    cb[11] = ci.maxContentLightLevel;

    // extra: inputFormat (0=NV12, 1=RGBA16, 2=P010), staticRGBA, SDR white.
    cb[12] = m_inputFormat;
    cb[13] = m_isStaticTexture ? 1.0f : 0.0f;  // 1 = static RGBA texture

    // extra.z: SDR diffuse white (nits). ITU-R BT.2408 defines SDR diffuse
    // white as 100 nits; HDR reference/paper white (adj.w, 203 nits) is a
    // different anchor and must not be used to scale SDR content.
    // (Replaces the old rounded-out 16th DWORD — audit 5.6 #9.)
    cb[14] = m_sdrWhiteNits;

    if (!hdrOutput) {
        // SDR output: the tone curve takes over the `disp` lanes and
        // extra.z. The shader's HDR->SDR branch reads them as
        //   disp.x = source pivot (PQ)   disp.y = destination pivot (PQ)
        //   disp.z = slope at the pivot    disp.w = toe coefficient (Pa)
        //   extra.z = input peak (PQ)
        // The display peak / mastering metadata are only consumed by the
        // HDR-output branch, so they cannot collide here. (The root
        // signature budget is exactly 64/64 DWORDs - there is no room for
        // another constant lane.)
        cb[8]  = m_toneCurve.srcPivot;
        cb[9]  = m_toneCurve.dstPivot;
        cb[10] = m_toneCurve.slope;
        cb[11] = m_toneCurve.pa;
        cb[14] = m_toneCurve.inMax;
    }
}

// ─── Audio Spectrum ──────────────────────────────────────────────────────

void D3D12Renderer::UpdateAudioSpectrum(const std::array<float, 32>& spectrum) {
    // RenderFrame uploads b2 straight from m_currentSpectrum as root
    // constants — there is no mapped constant buffer to mirror into
    // (audit 5.6 #11).
    m_currentSpectrum = spectrum;
}

bool D3D12Renderer::CreateVertexBuffer() {
    Vertex vertices[] = {
        {-1.0f, -1.0f, 0.0f, 1.0f},
        { 1.0f, -1.0f, 1.0f, 1.0f},
        {-1.0f,  1.0f, 0.0f, 0.0f},
        { 1.0f,  1.0f, 1.0f, 0.0f}
    };

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC resDesc = {};
    resDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    resDesc.Width            = sizeof(vertices);
    resDesc.Height           = 1;
    resDesc.DepthOrArraySize = 1;
    resDesc.MipLevels        = 1;
    resDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resDesc.SampleDesc.Count = 1;

    m_vertexBuffer = m_device.CreateCommittedResource(
        heapProps, D3D12_HEAP_FLAG_NONE, resDesc, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!m_vertexBuffer) return false;

    void* mapped = nullptr;
    m_vertexBuffer->Map(0, nullptr, &mapped);
    memcpy(mapped, vertices, sizeof(vertices));
    m_vertexBuffer->Unmap(0, nullptr);

    return true;
}

void D3D12Renderer::EnsureVertexUVs(float sx, float sy) {
    if (sx == m_appliedUvX && sy == m_appliedUvY) return;
    if (!m_vertexBuffer) return;
    // State changes only (video switch / video<->static); stall the pipe
    // so no in-flight frame samples half-rewritten UVs.
    ActiveQueue().WaitForGPU();
    struct QuadVert { float px, py, u, v; };
    QuadVert verts[] = {
        {-1.0f, -1.0f, 0.0f, sy},
        { 1.0f, -1.0f, sx,   sy},
        {-1.0f,  1.0f, 0.0f, 0.0f},
        { 1.0f,  1.0f, sx,   0.0f},
    };
    void* mapped = nullptr;
    if (SUCCEEDED(m_vertexBuffer->Map(0, nullptr, &mapped))) {
        memcpy(mapped, verts, sizeof(verts));
        m_vertexBuffer->Unmap(0, nullptr);
        m_appliedUvX = sx;
        m_appliedUvY = sy;
    }
}

// ─── Fill Mode Viewport ──────────────────────────────────────────────────

void D3D12Renderer::ComputeFillViewport(D3D12_VIEWPORT& vp, D3D12_RECT& scissor) const {
    float screenW = static_cast<float>(m_width);
    float screenH = static_cast<float>(m_height);

    vp.TopLeftX = 0.0f; vp.TopLeftY = 0.0f;
    vp.Width = screenW; vp.Height = screenH;
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;

    scissor.left = 0; scissor.top = 0;
    scissor.right = m_width; scissor.bottom = m_height;

    if (!m_hasTexture && !m_hasVideoFrame) return;

    const int texW = m_hasVideoFrame ? m_videoWidth  : m_texWidth;
    const int texH = m_hasVideoFrame ? m_videoHeight : m_texHeight;
    if (texW <= 0 || texH <= 0) return;

    float texAspect = static_cast<float>(texW) / static_cast<float>(texH);
    float screenAspect = screenW / screenH;

    switch (m_fillMode) {
        case FillMode::Stretch:
            break;
        case FillMode::Fit: {
            float fitW, fitH;
            if (texAspect > screenAspect) {
                fitW = screenW; fitH = screenW / texAspect;
            } else {
                fitH = screenH; fitW = screenH * texAspect;
            }
            vp.TopLeftX = (screenW - fitW) * 0.5f;
            vp.TopLeftY = (screenH - fitH) * 0.5f;
            vp.Width = fitW; vp.Height = fitH;
            break;
        }
        case FillMode::Fill: {
            float fillW, fillH;
            if (texAspect > screenAspect) {
                fillH = screenH; fillW = screenH * texAspect;
            } else {
                fillW = screenW; fillH = screenW / texAspect;
            }
            vp.TopLeftX = (screenW - fillW) * 0.5f;
            vp.TopLeftY = (screenH - fillH) * 0.5f;
            vp.Width = fillW; vp.Height = fillH;
            break;
        }
        case FillMode::Center: {
            float cx = screenW * 0.5f;
            float cy = screenH * 0.5f;
            float hw = static_cast<float>(texW) * 0.5f;
            float hh = static_cast<float>(texH) * 0.5f;
            vp.TopLeftX = cx - hw; vp.TopLeftY = cy - hh;
            vp.Width = static_cast<float>(texW);
            vp.Height = static_cast<float>(texH);
            scissor.left   = std::max(0L, static_cast<LONG>(vp.TopLeftX));
            scissor.top    = std::max(0L, static_cast<LONG>(vp.TopLeftY));
            scissor.right  = std::min(static_cast<LONG>(m_width),  static_cast<LONG>(vp.TopLeftX + vp.Width));
            scissor.bottom = std::min(static_cast<LONG>(m_height), static_cast<LONG>(vp.TopLeftY + vp.Height));
            break;
        }
        case FillMode::Tile:
            break;
    }
}

} // namespace rex::wallpaper
