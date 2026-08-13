#pragma once

#include "Device.hpp"
#include "SwapChain.hpp"
#include "CommandQueue.hpp"
#include "DescriptorHeapManager.hpp"
#include "wallpaper/decoder/WICTextureLoader.hpp"
#include "wallpaper/decoder/FfmpegVideoDecoder.hpp"

#include <shared/Result.hpp>
#include <rex/wallpaper/IWallpaperEngine.hpp>

#include <array>
#include <mutex>
#include <atomic>
#include <string_view>
#include <DirectXMath.h>

namespace rex::wallpaper {
using namespace rex::graphics;

// ─── D3D12Renderer ─────────────────────────────────────────────────────────
//
// Wallpaper-focused D3D12 renderer. Renders a fullscreen textured quad
// with configurable fill modes. For video playback, supports efficient
// per-frame texture updates via UpdateTexturePixels().
//
// Uses the shared rex::graphics primitives (Device, SwapChain, CommandQueue).

class D3D12Renderer {
public:
    D3D12Renderer() = default;
    ~D3D12Renderer();

    shared::Result<void> Initialize(HWND targetWindow, int width, int height);
    /// Initialize on an existing device (multi-monitor compositors share
    /// the engine's device so shared-handle video textures stay valid).
    shared::Result<void> Initialize(HWND targetWindow, int width, int height,
                                    ID3D12Device* adoptDevice);
    void Shutdown();
    void Resize(int width, int height);

    // ─── Texture Loading ────────────────────────────────────────────────
    shared::Result<void> LoadTexture(std::string_view path);
    shared::Result<void> UpdateTexturePixels(
        const uint8_t* pixels, int width, int height, int rowPitch);
    void ClearTexture();
    bool HasTexture() const { return m_hasTexture; }

    // ─── Video (shared-handle) rendering ────────────────────────────────
    /// Binds the GPU frame described by `frame` (a D3D11 pool slot shared
    /// via NT handle) for the next RenderFrame(). `waitFence` is the shared
    /// D3D11/D3D12 fence the command queue must wait on (value
    /// frame.fenceValue) before sampling. `frame.colorInfo` drives the
    /// color pipeline (SDR/PQ/HLG decode, primaries, HDR metadata).
    /// MUST be called between BeginFrame() and EndFrame() (same convention
    /// as UpdateTexturePixels).
    shared::Result<void> DrawVideoFrame(const VideoFrame& frame,
                                        ID3D12Fence* waitFence);
    /// Drops all opened video slot resources. MUST NOT be called between
    /// BeginFrame() and EndFrame() (it waits for the GPU).
    void ClearVideoFrame();
    bool HasVideoFrame() const { return m_hasVideoFrame; }

    /// HDR status and display luminance (from the swap chain; valid only
    /// when HDR is active — see SwapChain::IsHDRActive / GetDisplayMaxNits).
    bool  IsHDRActive() const { return m_swapChain.IsHDRActive(); }
    float GetDisplayMaxNits() const { return m_swapChain.GetDisplayMaxNits(); }

    // ─── Fill Mode ──────────────────────────────────────────────────────
    void SetFillMode(FillMode mode) { m_fillMode = mode; }
    FillMode GetFillMode() const { return m_fillMode; }

    // ─── Video Adjustments ──────────────────────────────────────────────
    /// Brightness/contrast/saturation applied by the video shader in
    /// linear space. Identity for all three is 1.0 (the engine clamps
    /// user values to 0..2).
    void SetVideoAdjustments(float brightness, float contrast, float saturation) {
        m_brightness = brightness;
        m_contrast   = contrast;
        m_saturation = saturation;
    }

    // ─── Transform (rotation / flip) ────────────────────────────────────
    /// Rotation in degrees around the screen center + horizontal/vertical
    /// mirroring. Applied to the quad in clip space via a vertex-shader
    /// matrix (root constant b1).
    void SetRotation(float degrees);
    void SetFlip(bool horizontal, bool vertical);

    // ─── Custom pixel shader hook ───────────────────────────────────────
    /// Compiles `hlslSource` (entry "main", ps_5_0) and swaps it into the
    /// pipeline. The shader must bind the same t0 texture / s0 sampler /
    /// b0 color constants as the default PS (the root signature is
    /// unchanged). Reverts to the default on compile/PSO failure.
    bool SetCustomPixelShader(const std::string& hlslSource);
    /// Revert to the built-in SDR/HDR pixel shader.
    void ClearCustomPixelShader();

    // ─── Frame Rendering ────────────────────────────────────────────────
    void BeginFrame();
    void RenderFrame();
    void EndFrame();
    void SetClearColor(float r, float g, float b);

    bool IsInitialized()   const { return m_initialized; }
    bool IsDeviceRemoved() const { return m_deviceRemoved; }

    // ─── Telemetry counters (for the memory-retention diagnostics) ─────
    uint64_t GetTextureBytes()        const { return m_textureBytes; }
    uint64_t GetUploadBytes()         const { return m_uploadBytes; }
    uint64_t GetTextureRecreations()  const { return m_textureRecreations; }
    uint64_t GetUploadCount()         const { return m_uploads; }
    uint64_t GetAllocatorsPerUpload() const { return m_allocatorsPerUpload; }

    // ─── Access to shared graphics primitives ──────────────────────────
    graphics::Device&       GetDevice()       { return m_device; }
    const graphics::Device& GetDevice() const { return m_device; }

private:
    // ─── Initialization helpers ─────────────────────────────────────────
    bool CreateRootSignature();
    bool CreatePipelineState(DXGI_FORMAT rtvFormat,
                             Microsoft::WRL::ComPtr<ID3D12PipelineState>& outPs);
    bool CreatePipelineStateWithPs(DXGI_FORMAT rtvFormat,
                                   const void* psBytecode, SIZE_T psSize,
                                   Microsoft::WRL::ComPtr<ID3D12PipelineState>& outPs);
    bool CreateVertexBuffer();

    /// Rebuilds m_transformMatrix from rotation/flip (row-major, matches
    /// the vertex shader's row-vector mul(v, M)). Caller holds m_renderMutex.
    void UpdateTransformMatrix();

    // ─── Color pipeline ─────────────────────────────────────────────────
    /// Fills the root-constant payload (b0) from the active frame's
    /// colorimetry plus display settings.
    void BuildColorConstants(float cb[16], bool hdrOutput) const;

    // ─── Texture Upload ────────────────────────────────────────────────
    bool UploadTextureToGPU(const TextureData& data);

    // ─── Fill Mode Viewport ────────────────────────────────────────────
    void ComputeFillViewport(D3D12_VIEWPORT& vp, D3D12_RECT& scissor) const;

    // ─── Shared primitives ─────────────────────────────────────────────
    graphics::Device       m_device;
    graphics::SwapChain    m_swapChain;
    graphics::CommandQueue m_commandQueue;

    // ─── Per-frame command state ───────────────────────────────────────
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    m_commandAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commandList;

    // ─── Pipeline state ────────────────────────────────────────────────
    Microsoft::WRL::ComPtr<ID3D12RootSignature>  m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_pipelineState;     // SDR back buffer
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_pipelineStateHdr;  // R16F back buffer
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_customPSO;         // custom PS (SDR)
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_customPSOHdr;      // custom PS (R16F)
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_vertexBuffer;

    // ─── SRV heap ──────────────────────────────────────────────────────
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap;

    // ─── Texture state ─────────────────────────────────────────────────
    Microsoft::WRL::ComPtr<ID3D12Resource>  m_texture;
    Microsoft::WRL::ComPtr<ID3D12Resource>  m_textureUploadBuffer;
    D3D12_GPU_DESCRIPTOR_HANDLE            m_textureSRVGpuHandle{};
    bool m_hasTexture = false;
    int  m_texWidth  = 0;
    int  m_texHeight = 0;

    // ─── Video (shared-handle) state ───────────────────────────────────
    // Opened D3D12 views of the decoder's BGRA pool slots. One SRV per
    // slot, laid out in the SRV heap after the static texture (slots 1..8).
    struct VideoSlotResource {
        HANDLE sharedHandle = nullptr;
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        D3D12_GPU_DESCRIPTOR_HANDLE srvGpu{};
    };
    static constexpr UINT kMaxVideoSlots = 8;
    std::array<VideoSlotResource, kMaxVideoSlots> m_videoSlots;
    UINT   m_videoSlotCount = 0;
    HANDLE m_activeVideoHandle = nullptr;
    bool   m_hasVideoFrame = false;
    int    m_videoWidth  = 0;
    int    m_videoHeight = 0;

    // ─── Color pipeline state ──────────────────────────────────────────
    VideoColorInfo m_activeColorInfo;   // colorimetry of the bound frame
    bool m_isHdrOutput = false;         // swap chain is R16F + PQ/BT.2020
    float m_displayPeakNits = 80.0f;    // tone-mapping target (display or
                                        // SDR reference white in nits)

    // ─── Fill mode ─────────────────────────────────────────────────────
    FillMode m_fillMode = FillMode::Fill;

    // ─── Transform state ───────────────────────────────────────────────
    float m_rotation = 0.0f;
    bool  m_flipH = false;
    bool  m_flipV = false;
    DirectX::XMFLOAT4X4 m_transformMatrix = {
        1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

    // ─── Custom pixel shader state ─────────────────────────────────────
    std::string m_customPsSource;
    bool m_customPsActive = false;

    // ─── Video adjustments (identity = 1.0) ────────────────────────────
    float m_brightness = 1.0f;
    float m_contrast   = 1.0f;
    float m_saturation = 1.0f;

    // ─── Dimensions & state ───────────────────────────────────────────
    int   m_width = 0;
    int   m_height = 0;
    float m_clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    bool  m_initialized  = false;
    bool  m_deviceRemoved = false;

    // ─── Thread safety ────────────────────────────────────────────────
    mutable std::mutex m_renderMutex;

    // ─── Telemetry ─────────────────────────────────────────────────────
    std::atomic<uint64_t> m_textureBytes{0};
    std::atomic<uint64_t> m_uploadBytes{0};
    std::atomic<uint64_t> m_textureRecreations{0};
    std::atomic<uint64_t> m_uploads{0};
    std::atomic<uint64_t> m_allocatorsPerUpload{2}; // copy list + render frame

    // ─── Vertex layout ────────────────────────────────────────────────
    struct Vertex { float x, y, u, v; };
};

} // namespace rex::wallpaper
