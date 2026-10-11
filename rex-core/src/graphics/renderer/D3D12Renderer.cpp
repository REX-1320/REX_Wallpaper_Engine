
#include "D3D12Renderer.hpp"
#include "graphics/renderer/HdrSyncPolicy.hpp"
#include "wallpaper/decoder/WICTextureLoader.hpp"
#include <shared/Logger.hpp>
#include <d3dcompiler.h>

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

static const char* g_pixelShaderSource = R"(
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
                    // y = debugMode  (0=normal, 1=Y-only, 2=UV-only)
                    // z = staticRGBA (1 = static RGBA texture, skip YUV decode)
};

cbuffer AudioConstants : register(b2) {
    float4 spec[8];
};

struct VSOutput {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD;
};

static const float3x3 BT2020_TO_BT709 = {
    1.6605, -0.1246, -0.0182,
    -0.5876, 1.1323, -0.1006,
    -0.0728, -0.0077, 1.1187
};
static const float3x3 BT709_TO_BT2020 = {
    0.6274, 0.0691, 0.0164,
    0.3293, 0.9195, 0.0880,
    0.0433, 0.0114, 0.8956
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

float3 AcesFit(float3 x) {
    return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
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

    if (extra.z > 0.5) {
        float4 c = texY.Sample(sam, input.uv);
        rgb = (mode.y > 0.5) ? (PqToLinear(saturate(c.rgb)) / norm)
                             : SrgbToLinear(c.rgb);
    } else if (extra.x > 1.5) {
        float  yRaw  = texY.Sample(sam, input.uv).r;
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
        float r = yN + 1.4746 * crN;
        float g = yN - 0.16455 * cbN - 0.57135 * crN;
        float b = yN + 1.8814 * cbN;
        rgb = float3(r, g, b);
        if (mode.y > 1.5) {
            rgb = HlgToLinear(saturate(rgb)) / norm;
        } else {
            rgb = PqToLinear(saturate(rgb)) / norm;
        }
        rgb = max(mul(rgb, BT2020_TO_BT709), 0.0);
    } else if (extra.x > 0.5) {
        // RGB video slot (P0-3: software-decode BGRA/RGBA16 pool): already
        // RGB, skip the YUV matrix. Mirrors the static-RGBA branch above
        // plus the P010 branch's transfer split (HLG vs PQ vs SDR).
        float4 c = texY.Sample(sam, input.uv);
        if (mode.y > 1.5) {
            rgb = HlgToLinear(saturate(c.rgb)) / norm;
        } else if (mode.y > 0.5) {
            rgb = PqToLinear(saturate(c.rgb)) / norm;
        } else {
            rgb = SrgbToLinear(c.rgb);
        }
    } else if (mode.y > 0.5) {
        float4 c = texY.Sample(sam, input.uv);
        rgb = PqToLinear(saturate(c.rgb)) / norm;
    } else {
        float  y  = texY.Sample(sam, input.uv).r;
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
        const bool hdrInput = (mode.y > 0.5) || (extra.x > 1.5);
        rgb = rgb * (adj.x * adj.y);
        rgb = hdrInput ? max(rgb, 0.0) : saturate(rgb);
        float luma = dot(rgb, float3(0.2126, 0.7152, 0.0722));
        rgb = lerp(luma, rgb, adj.z);
    }

    if (extra.y > 0.5 && extra.z < 0.5) {
        if (extra.y < 1.5) {
            if (extra.x > 1.5) {
                float yRaw = texY.Sample(sam, input.uv).r;
                return float4(yRaw, yRaw, yRaw, 1.0);
            } else {
                float y = texY.Sample(sam, input.uv).r;
                return float4(y, y, y, 1.0);
            }
        } else {
            if (extra.x > 1.5) {
                float2 uvRaw = texUV.Sample(sam, input.uv).rg;
                return float4(uvRaw.x, uvRaw.y, 0.5, 1.0);
            } else {
                float2 uv = texUV.Sample(sam, input.uv).rg;
                return float4(uv.x, uv.y, 0.5, 1.0);
            }
        }
    }

    if (mode.w > 0.5) {
        float3 lin;
        if (mode.y > 0.5 || extra.x > 1.5) {
            // HDR input: PQ/HLG decode already yields absolute luminance, which
            // was divided by `norm` (paper white) on the way in, so scale it
            // back and cap at the display peak.
            lin = min(rgb * min(norm, peak), peak);
        } else {
            // SDR input: rgb is linear with 1.0 == SDR white, which maps to
            // paper white (cb[7]). The mastering display max (cb[9]) has no
            // meaning for SDR content -- it falls back to a 1000 nit default.
            lin = rgb * adj.w;
        }
        return float4(LinearToPq((mode.y > 0.5 || extra.x > 1.5) ? lin : mul(lin, BT709_TO_BT2020)), 1.0);
    }

    if (mode.y > 0.5 || extra.x > 1.5) {
        rgb = AcesFit(rgb);
    }
    return float4(LinearToSrgb(saturate(rgb)), 1.0);
}
)";

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
    if (m_initialized) {
        return shared::Result<void>(shared::ErrorCode::AlreadyInitialized,
                                    "Renderer already initialized");
    }

    REX_LOG_INFO("D3D12", "Initializing D3D12 renderer ({}x{}{})", width, height,
                 adoptDevice ? " on adopted device" : "");

    m_width  = width;
    m_height = height;

    // ── Device ─────────────────────────────────────────────────────────
    auto devResult = adoptDevice ? m_device.InitializeAdopted(adoptDevice)
                                 : m_device.Initialize();
    if (devResult.IsError()) {
        return devResult;
    }

    // ── Command Queue ──────────────────────────────────────────────────
    auto qResult = m_commandQueue.Initialize(m_device.Get());
    if (qResult.IsError()) {
        return qResult;
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

    // ── SRV heap (shader-visible, 64 slots) ───────────────────────────
    m_srvHeap = m_device.CreateDescriptorHeap(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 64,
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

    // ── Vertex buffer ─────────────────────────────────────────────────
    if (!CreateVertexBuffer()) {
        return shared::Result<void>(shared::ErrorCode::UnknownError,
                                    "Failed to create vertex buffer");
    }

    // ── Pipeline state ────────────────────────────────────────────────
    // One PSO per back-buffer format: SDR (8-bit UNORM) and HDR (16-bit
    // float). The shader branches on the HDR flag via root constants.
    if (!CreatePipelineState(DXGI_FORMAT_R8G8B8A8_UNORM, m_pipelineState)) {
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

    // ── Audio spectrum constant buffer (32 floats, b2) ───────────────
    {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = sizeof(std::array<float, 32>);
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.SampleDesc.Count = 1;
        HRESULT hr = m_device.Get()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE,
            &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&m_audioCB));
        if (SUCCEEDED(hr)) {
            m_audioCB->Map(0, nullptr, reinterpret_cast<void**>(&m_mappedSpectrum));
            m_currentSpectrum.fill(0.0f);
        }
    }

    m_initialized = true;
    REX_LOG_INFO("D3D12", "Renderer initialized successfully");
    return shared::Result<void>();
}

shared::Result<void> D3D12Renderer::Initialize(HWND targetWindow, int width, int height,
                                               ID3D12Device* adoptDevice,
                                               graphics::CommandQueue* sharedQueue) {
    if (m_initialized) {
        return shared::Result<void>(shared::ErrorCode::AlreadyInitialized,
                                    "Renderer already initialized");
    }
    if (!sharedQueue || !sharedQueue->Get()) {
        return shared::Result<void>(shared::ErrorCode::InvalidArgument,
                                    "shared queue is null");
    }

    REX_LOG_INFO("D3D12", "Initializing D3D12 renderer ({}x{}, shared queue)", width, height);

    m_width  = width;
    m_height = height;
    m_sharedQueue = sharedQueue;

    // ── Device ─────────────────────────────────────────────────────────
    auto devResult = adoptDevice ? m_device.InitializeAdopted(adoptDevice)
                                 : m_device.Initialize();
    if (devResult.IsError()) {
        return devResult;
    }

    // Skip local command queue — use the shared one.
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
    if (m_forceSdr) {
        m_swapChain.SetForceSdr(true);
    }
    auto scResult = m_swapChain.Initialize(
        m_sharedQueue->Get(), m_device.Get(),
        targetWindow, width, height);
    if (scResult.IsError()) {
        return scResult;
    }

    // ── SRV heap (shader-visible, 64 slots) ───────────────────────────
    m_srvHeap = m_device.CreateDescriptorHeap(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 64,
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

    // ── Vertex buffer ─────────────────────────────────────────────────
    if (!CreateVertexBuffer()) {
        return shared::Result<void>(shared::ErrorCode::UnknownError,
                                    "Failed to create vertex buffer");
    }

    // ── Pipeline state ────────────────────────────────────────────────
    if (!CreatePipelineState(DXGI_FORMAT_R8G8B8A8_UNORM, m_pipelineState)) {
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
        m_copyContexts[i].list->Close();
    }

    m_initialized = true;
    REX_LOG_INFO("D3D12", "Renderer initialized with shared queue");
    return shared::Result<void>();
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
    m_hasVideoFrame = false;

    // Unmap audio spectrum CB
    if (m_mappedSpectrum) {
        m_audioCB->Unmap(0, nullptr);
        m_mappedSpectrum = nullptr;
    }
    m_audioCB.Reset();
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

    // Locate or open the pool slot resource (handles are stable per video
    // open; the decoder's pool has kPoolSize slots, so the cache never
    // needs eviction).
    UINT idx = kMaxVideoSlots;
    for (UINT i = 0; i < m_videoSlotCount; ++i) {
        if (m_videoSlots[i].sharedHandle == frame.sharedHandle) {
            idx = i;
            break;
        }
    }
    if (idx == kMaxVideoSlots) {
        if (m_videoSlotCount >= kMaxVideoSlots) {
            return shared::Result<void>(shared::ErrorCode::InvalidState,
                                        "Video slot cache exhausted");
        }
        idx = m_videoSlotCount;
        HRESULT hr = dev->OpenSharedHandle(frame.sharedHandle,
                                           IID_PPV_ARGS(&m_videoSlots[idx].resource));
        if (FAILED(hr)) {
            if (IsDeviceRemovedHr(hr)) m_deviceRemoved = true;
            return shared::Result<void>(shared::ErrorCode::DeviceLost,
                                        "OpenSharedHandle failed");
        }
        m_videoSlots[idx].sharedHandle = frame.sharedHandle;

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

        // Set shader input format constant for the current frame.
        // 0 = NV12, 1 = RGBA16, 2 = P010
        if (fmt == DXGI_FORMAT_P010) {
            m_inputFormat = 2.0f;
        } else if (fmt == DXGI_FORMAT_NV12) {
            m_inputFormat = 0.0f;
        } else {
            m_inputFormat = 1.0f;  // BGRA / RGBA16F
        }

        const UINT inc = dev->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu =
            m_srvHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE gpu =
            m_srvHeap->GetGPUDescriptorHandleForHeapStart();

        if (fmt == DXGI_FORMAT_NV12 || fmt == DXGI_FORMAT_P010) {
            // NV12/P010: create two SRVs — t0 = Y plane, t1 = UV plane.
            // Descriptor layout: Y at (1 + idx*2), UV at (1 + idx*2 + 1).
            const bool is10bit = (fmt == DXGI_FORMAT_P010);
            const UINT base = 1 + idx * 2;

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
            const UINT base = 1 + idx * 2;

            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = fmt;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            if (fmt == DXGI_FORMAT_B8G8R8A8_UNORM) {
                srvDesc.Shader4ComponentMapping =
                    D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
                        D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2,
                        D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1,
                        D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0,
                        D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_3);
            } else {
                srvDesc.Shader4ComponentMapping =
                    D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            }
            srvDesc.Texture2D.MipLevels = 1;
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

        m_videoSlotCount++;
    }

    // GPU ordering: the D3D11 video engine blts into this pool slot and
    // signals the shared fence; the D3D12 queue must wait on that signal
    // before the draw in RenderFrame() samples the slot.
    if (waitFence) {
        ActiveQueue().WaitForFence(waitFence, frame.fenceValue);
    }

    m_activeVideoHandle = frame.sharedHandle;
    m_hasVideoFrame = true;
    m_videoWidth  = frame.width;
    m_videoHeight = frame.height;
    m_activeColorInfo = frame.colorInfo;

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

    ActiveQueue().WaitForGPU();
    for (auto& slot : m_videoSlots) {
        slot.resource.Reset();
        slot.sharedHandle = nullptr;
        slot.srvGpu = {};
    }
    m_videoSlotCount = 0;
    m_activeVideoHandle = nullptr;
    m_hasVideoFrame = false;
    m_videoWidth  = 0;
    m_videoHeight = 0;
    m_videoUvScaleX = 1.0f;
    m_videoUvScaleY = 1.0f;
    m_activeColorInfo = VideoColorInfo{};
    m_inputFormat = 0.0f;
}

// ─── Update Texture Pixels (video frames) ────────────────────────────────

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
        ctx.list->Close();
    }

    allocatorOut = ctx.allocator;
    listOut      = ctx.list;
    return shared::Result<void>();
}

bool D3D12Renderer::BeginFrame() {
    m_renderMutex.lock();
    // R1: arm the reentry detector while the frame lock is held; EndFrame
    // (and the early-out below) disarm it before unlocking.
    m_frameLock.EnterFrame();

    m_swapChain.WaitForFrame();

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
            m_renderMutex.unlock();
            return false;
        }
    }
    ctx.allocator->Reset();
    m_commandList->Reset(ctx.allocator.Get(), m_pipelineState.Get());
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
            if (m_videoSlots[i].sharedHandle == m_activeVideoHandle) {
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

        m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());
        ID3D12PipelineState* pso = nullptr;
        // --force-sdr: always use SDR PSO (R8G8B8A8_UNORM) even if swap chain
        // is R16F, because our NV12 shader outputs sRGB, not PQ.
        const bool useHdrPso = m_forceSdr ? false : m_isHdrOutput;
        if (m_customPsActive) {
            pso = useHdrPso ? m_customPSOHdr.Get() : m_customPSO.Get();
        }
        if (!pso) {
            pso = useHdrPso ? m_pipelineStateHdr.Get()
                            : m_pipelineState.Get();
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
        float cb[15] = {};
        BuildColorConstants(cb, m_isHdrOutput);
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

    // Transition back to present
    barrier.Transition.pResource   = backBuffer;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    m_commandList->ResourceBarrier(1, &barrier);
}

void D3D12Renderer::EndFrame() {
    m_commandList->Close();
    ActiveQueue().Execute(m_commandList.Get());

    HRESULT hr = m_swapChain.Present(1, 0);

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
    m_renderMutex.unlock();
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
    if (action.updateFlag) {
        m_isHdrOutput = m_swapChain.IsHDR();
        REX_LOG_INFO("D3D12", "HDR output {} (swapchain {})",
                     m_isHdrOutput ? "enabled" : "disabled",
                     m_isHdrOutput ? "R16F/PQ" : "UNORM/sRGB");
    }
    if (action.createHdrPso) {
        if (!CreatePipelineState(DXGI_FORMAT_R16G16B16A16_FLOAT,
                                 m_pipelineStateHdr)) {
            REX_LOG_ERROR("D3D12",
                          "HDR PSO creation failed; staying on SDR output");
            m_isHdrOutput = false;
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

    // Upload buffer
    UINT64 uploadSize = static_cast<UINT64>(data.rowPitch) * data.height;
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
            memcpy(dst, src, static_cast<size_t>(data.rowPitch));
            src += data.rowPitch;
            dst += data.rowPitch;
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
    srcLoc.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(data.rowPitch);

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

    // b0: color constants (mode / adjustments / display metadata) —
    // 15 floats, updated per frame via SetGraphicsRoot32BitConstants.
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

    HRESULT hr = D3DCompile(g_pixelShaderSource, strlen(g_pixelShaderSource),
           nullptr, nullptr, nullptr, "main", "ps_5_0",
           D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psBlob, &errorBlob);
    if (FAILED(hr)) {
        REX_LOG_ERROR("D3D12", "Pixel shader compile failed");
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

void D3D12Renderer::BuildColorConstants(float cb[15], bool hdrOutput) const {
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

    // extra: inputFormat (0=NV12, 1=RGBA16, 2=P010), debugMode, staticRGBA
    cb[12] = m_inputFormat;
    cb[13] = m_debugMode;
    cb[14] = m_isStaticTexture ? 1.0f : 0.0f;  // 1 = static RGBA texture
}

// ─── Audio Spectrum ──────────────────────────────────────────────────────

void D3D12Renderer::UpdateAudioSpectrum(const std::array<float, 32>& spectrum) {
    m_currentSpectrum = spectrum;
    if (m_mappedSpectrum) {
        *m_mappedSpectrum = spectrum;
    }
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
