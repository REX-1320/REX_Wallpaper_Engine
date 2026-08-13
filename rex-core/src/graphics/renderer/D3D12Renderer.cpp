
#include "D3D12Renderer.hpp"
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
Texture2D tex : register(t0);
SamplerState sam : register(s0);

// Root constants (16 floats = 4 float4, set per frame from the decoded
// frame's colorimetry):
//   mode: x = yuv matrix (0=BT.601, 1=BT.709, 2=BT.2020)
//         y = transfer  (0=SDR, 1=PQ/ST.2084, 2=HLG)
//         z = full range flag
//         w = HDR output flag (swap chain is R16F + PQ/BT.2020)
//   adj : x = brightness (gain, identity 1.0), y = contrast (identity 1.0),
//         z = saturation (identity 1.0),
//         w = paper white in nits (SDR white on HDR output)
//   disp: x = display peak nits, y = mastering max nits,
//         z = mastering min nits, w = MaxCLL
cbuffer ColorConstants : register(b0) {
    float4 mode;
    float4 adj;
    float4 disp;
    float4 unused;
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

// ST.2084 (PQ) EOTF / inverse EOTF. Input/output in 10000-nit units.
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

// sRGB / BT.709 gamma (G22) decode & encode. The D3D11 video processor
// stores SDR RGB gamma-encoded; treating G22 as sRGB is the standard
// approximation.
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

// Simple filmic shoulder (ACES-fit style) for HDR -> SDR tone mapping.
float3 AcesFit(float3 x) {
    return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
}

float4 main(VSOutput input) : SV_TARGET {
    float4 c = tex.Sample(sam, input.uv);

    const float masterMax = max(disp.y, 1.0);
    const float peak = max(disp.x, 100.0);

    // ── Decode source to linear light (BT.709 primaries) ───────────────
    // The decoder's pool already holds RGB (YUV conversion happens in the
    // D3D11 video processor / swscale): SDR = G22-encoded BT.709 (8-bit),
    // HDR = PQ-encoded BT.2020 in 16-bit float (the video processor
    // converts HLG sources to PQ — this SDK has no RGB HLG output space).
    float3 rgb;   // linear, BT.709, relative to masterMax (HDR) or SDR white
    if (mode.y > 0.5) {          // PQ or HLG (both arrive as PQ)
        rgb = mul(BT2020_TO_BT709, PqToLinear(c.rgb)) / masterMax;
    } else {                     // SDR
        rgb = SrgbToLinear(c.rgb);
    }

    // ── User adjustments (linear space) ────────────────────────────────
    // brightness/contrast combine into a single gain (identity 1.0/1.0);
    // saturation is a luma-weighted mix (identity 1.0).
    rgb = saturate(rgb * (adj.x * adj.y));
    float luma = dot(rgb, float3(0.2126, 0.7152, 0.0722));
    rgb = lerp(luma, rgb, adj.z);

    // SDR content on an SDR target with neutral adjustments: exact passthrough.
    if (mode.y <= 0.5 && mode.w <= 0.5 &&
        adj.x == 1.0 && adj.y == 1.0 && adj.z == 1.0) {
        return c;
    }

    if (mode.w > 0.5) {
        // ── HDR output: tone map to display peak, PQ-encode BT.2020 ───
        // HDR source: absolute nits scaled to mastering peak. SDR source:
        // white (1.0) maps to the SDR reference white level (paper white).
        float3 lin = (mode.y > 0.5) ? rgb * masterMax : rgb * adj.w;
        lin = min(lin * min(1.0, peak / masterMax), peak);  // scale-to-fit
        return float4(LinearToPq(mul(BT709_TO_BT2020, lin)), 1.0);
    }

    // ── SDR output ─────────────────────────────────────────────────────
    if (mode.y > 0.5) {
        rgb = AcesFit(rgb * 1.0);                 // HDR → SDR tone map
        return float4(LinearToSrgb(rgb), 1.0);
    }
    return float4(LinearToSrgb(rgb), 1.0);
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

    // ── Command allocator + list ───────────────────────────────────────
    m_commandAllocator = m_device.CreateCommandAllocator();
    if (!m_commandAllocator) {
        return shared::Result<void>(shared::ErrorCode::UnknownError,
                                    "Failed to create command allocator");
    }

    // ── Swap Chain ─────────────────────────────────────────────────────
    auto scResult = m_swapChain.Initialize(
        m_commandQueue.Get(), m_device.Get(),
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
        m_commandAllocator.Get(), m_pipelineState.Get());
    if (!m_commandList) {
        return shared::Result<void>(shared::ErrorCode::UnknownError,
                                    "Failed to create command list");
    }
    m_commandList->Close();

    m_initialized = true;
    REX_LOG_INFO("D3D12", "Renderer initialized successfully");
    return shared::Result<void>();
}

void D3D12Renderer::Shutdown() {
    if (!m_initialized) return;

    REX_LOG_INFO("D3D12", "Shutting down renderer...");

    std::lock_guard<std::mutex> lock(m_renderMutex);
    m_commandQueue.WaitForGPU();

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
    m_activeColorInfo = VideoColorInfo{};
    m_vertexBuffer.Reset();
    m_pipelineState.Reset();
    m_pipelineStateHdr.Reset();
    m_customPSO.Reset();
    m_customPSOHdr.Reset();
    m_customPsActive = false;
    m_rootSignature.Reset();
    m_srvHeap.Reset();
    m_commandList.Reset();
    m_commandAllocator.Reset();

    // Shutdown shared primitives (order: swap chain → queue → device)
    m_swapChain.Shutdown();
    m_commandQueue.Shutdown();
    // Device has no persistent state to clean up

    m_initialized = false;
    REX_LOG_INFO("D3D12", "Renderer shutdown complete");
}

// ─── Texture Loading ──────────────────────────────────────────────────────

shared::Result<void> D3D12Renderer::LoadTexture(std::string_view path) {
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
        m_commandQueue.WaitForGPU();
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
        m_commandQueue.WaitForGPU();
        m_texture.Reset();
        m_textureUploadBuffer.Reset();
        m_hasTexture = false;
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

        // SRV format follows the pool texture the decoder created
        // (B8G8R8A8_UNORM for SDR, R16G16B16A16_FLOAT for HDR). Reading it
        // from the opened resource keeps the renderer format-agnostic.
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = m_videoSlots[idx].resource->GetDesc().Format;
        srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels     = 1;

        const UINT inc = dev->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu =
            m_srvHeap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += (1 + idx) * inc;
        dev->CreateShaderResourceView(m_videoSlots[idx].resource.Get(),
                                      &srvDesc, cpu);
        m_videoSlots[idx].srvGpu =
            m_srvHeap->GetGPUDescriptorHandleForHeapStart();
        m_videoSlots[idx].srvGpu.ptr += (1 + idx) * inc;

        m_videoSlotCount++;
    }

    // GPU ordering: the D3D11 video engine blts into this pool slot and
    // signals the shared fence; the D3D12 queue must wait on that signal
    // before the draw in RenderFrame() samples the slot.
    if (waitFence) {
        m_commandQueue.WaitForFence(waitFence, frame.fenceValue);
    }

    m_activeVideoHandle = frame.sharedHandle;
    m_hasVideoFrame = true;
    m_videoWidth  = frame.width;
    m_videoHeight = frame.height;
    m_activeColorInfo = frame.colorInfo;

    // Tone-mapping target: the actual display peak when HDR is active,
    // otherwise SDR reference white (80 nits).
    m_displayPeakNits = m_swapChain.IsHDRActive()
                            ? m_swapChain.GetDisplayMaxNits()
                            : 80.0f;
    return shared::Result<void>();
}

void D3D12Renderer::ClearVideoFrame() {
    std::lock_guard<std::mutex> lock(m_renderMutex);

    m_commandQueue.WaitForGPU();
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
    m_activeColorInfo = VideoColorInfo{};
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

    if (!m_texture || m_texWidth != width || m_texHeight != height) {
        if (m_texture) {
            m_commandQueue.WaitForGPU();
            m_texture.Reset();
            m_textureUploadBuffer.Reset();
        }
        m_textureRecreations++;

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

    // Create temp copy command list
    auto copyAllocator = m_device.CreateCommandAllocator();
    if (!copyAllocator) {
        if (IsDeviceRemovedHr(m_device.GetDeviceRemovedReason())) m_deviceRemoved = true;
        return shared::Result<void>(shared::ErrorCode::UnknownError,
                                    "CreateCommandAllocator failed");
    }

    auto copyList = m_device.CreateCommandList(copyAllocator.Get());
    if (!copyList) {
        if (IsDeviceRemovedHr(m_device.GetDeviceRemovedReason())) m_deviceRemoved = true;
        return shared::Result<void>(shared::ErrorCode::UnknownError,
                                    "CreateCommandList failed");
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
    m_commandQueue.Execute(copyList.Get());
    m_commandQueue.WaitForGPU();

    m_uploads++;

    return shared::Result<void>();
}

// ─── Frame Rendering ──────────────────────────────────────────────────────

void D3D12Renderer::BeginFrame() {
    m_renderMutex.lock();

    m_swapChain.WaitForFrame();

    m_commandAllocator->Reset();
    m_commandList->Reset(m_commandAllocator.Get(), m_pipelineState.Get());
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
            barrier.Transition.pResource   = texToDraw;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            m_commandList->ResourceBarrier(1, &barrier);
        }

        m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());
        ID3D12PipelineState* pso = nullptr;
        if (m_customPsActive) {
            pso = m_isHdrOutput ? m_customPSOHdr.Get() : m_customPSO.Get();
        }
        if (!pso) {
            pso = m_isHdrOutput ? m_pipelineStateHdr.Get()
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
        float cb[16] = {};
        BuildColorConstants(cb, m_isHdrOutput);
        m_commandList->SetGraphicsRoot32BitConstants(1, 16, cb, 0);

        // Transform matrix (rotation/flip) for the vertex shader.
        m_commandList->SetGraphicsRoot32BitConstants(2, 16,
                                                     &m_transformMatrix, 0);

        m_commandList->DrawInstanced(4, 1, 0, 0);

        if (videoActive) {
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COMMON;
            m_commandList->ResourceBarrier(1, &barrier);
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
    m_commandQueue.Execute(m_commandList.Get());

    HRESULT hr = m_swapChain.Present(1, 0);

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
        // so the log isn't spammed at 60 fps.
        static thread_local int s_occludedCount = 0;
        s_occludedCount++;
        if (s_occludedCount % 300 == 1) {
            REX_LOG_WARN("D3D12",
                "Present returned DXGI_STATUS_OCCLUDED. "
                "DWM considers the render target fully occluded "
                "(frame count: {}).", s_occludedCount);
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

    m_commandQueue.Signal();
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

    m_commandQueue.WaitForGPU();
    m_swapChain.Resize(m_device.Get(), width, height);

    m_width  = width;
    m_height = height;
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

    // Copy via command list
    auto uploadAlloc = m_device.CreateCommandAllocator();
    if (!uploadAlloc) {
        if (IsDeviceRemovedHr(m_device.GetDeviceRemovedReason())) m_deviceRemoved = true;
        m_texture.Reset();
        return false;
    }

    auto uploadList = m_device.CreateCommandList(uploadAlloc.Get());
    if (!uploadList) {
        if (IsDeviceRemovedHr(m_device.GetDeviceRemovedReason())) m_deviceRemoved = true;
        m_texture.Reset();
        return false;
    }

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
    m_commandQueue.Execute(uploadList.Get());
    m_commandQueue.WaitForGPU();

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
    range.NumDescriptors        = 1;
    range.BaseShaderRegister    = 0;
    range.RegisterSpace         = 0;

    D3D12_ROOT_PARAMETER params[3] = {};

    // t0: texture (static or video slot)
    params[0].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges   = &range;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // b0: color constants (mode / adjustments / display metadata) —
    // 16 floats, updated per frame via SetGraphicsRoot32BitConstants.
    params[1].ParameterType                    = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister         = 0;
    params[1].Constants.RegisterSpace          = 0;
    params[1].Constants.Num32BitValues         = 16;
    params[1].ShaderVisibility                 = D3D12_SHADER_VISIBILITY_PIXEL;

    // b1: transform matrix (rotation/flip) — 16 floats, vertex shader.
    params[2].ParameterType                    = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants.ShaderRegister         = 1;
    params[2].Constants.RegisterSpace          = 0;
    params[2].Constants.Num32BitValues         = 16;
    params[2].ShaderVisibility                 = D3D12_SHADER_VISIBILITY_VERTEX;

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
    desc.NumParameters     = 3;
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
    psoDesc.BlendState.RenderTarget[0].BlendEnable       = TRUE;
    psoDesc.BlendState.RenderTarget[0].SrcBlend          = D3D12_BLEND_SRC_ALPHA;
    psoDesc.BlendState.RenderTarget[0].DestBlend         = D3D12_BLEND_INV_SRC_ALPHA;
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
    cb[3] = hdrOutput ? 1.0f : 0.0f;

    // adj: brightness | contrast | saturation | paper white
    cb[4] = m_brightness;                 // brightness (identity 1.0)
    cb[5] = m_contrast;                   // contrast (identity 1.0)
    cb[6] = m_saturation;                 // saturation (identity 1.0)
    cb[7] = 203.0f;                    // SDR reference white on HDR (nits)

    // disp: display peak | mastering max | mastering min | MaxCLL
    cb[8] = m_displayPeakNits;           // tone-mapping target (nits)
    cb[9] = ci.masteringDisplayMaxLuminance > 0.0f
                ? ci.masteringDisplayMaxLuminance : 1000.0f;
    cb[10] = ci.masteringDisplayMinLuminance;
    cb[11] = ci.maxContentLightLevel;

    cb[12] = cb[13] = cb[14] = cb[15] = 0.0f;
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
