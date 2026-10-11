#pragma once

#include "Device.hpp"
#include "SwapChain.hpp"
#include "CommandQueue.hpp"
#include "FrameContextPolicy.hpp"
#include "FrameLockPolicy.hpp"
#include "DescriptorHeapManager.hpp"
#include "ResourceStateTracker.hpp"
#include "wallpaper/decoder/WICTextureLoader.hpp"
#include "wallpaper/decoder/FfmpegVideoDecoder.hpp"
#include "wallpaper/engine/OcclusionPolicy.hpp"

#include <shared/Result.hpp>
#include <rex/wallpaper/IWallpaperEngine.hpp>

#include <array>
#include <mutex>
#include <atomic>
#include <span>
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
    /// Initialize sharing an existing CommandQueue (cross-renderer race fix:
    /// all renderers sharing a device must use the same queue to avoid
    /// concurrent resource state transitions on shared textures).
    shared::Result<void> Initialize(HWND targetWindow, int width, int height,
                                    ID3D12Device* adoptDevice,
                                    graphics::CommandQueue* sharedQueue);
    void Shutdown();
    void Resize(int width, int height);
    /// Re-evaluate the HDR output state (OS toggle, window moved outputs)
    /// and sync the cached flag + HDR PSO. Safe to call when nothing
    /// changed (idempotent). Also runs inside Resize.
    void RefreshHdrOutput();

    // ─── Texture Loading ────────────────────────────────────────────────
    shared::Result<void> LoadTexture(std::string_view path);
    shared::Result<void> UpdateTexturePixels(
        const uint8_t* pixels, int width, int height, int rowPitch);
    shared::Result<void> UpdateTexturePixels(
        std::span<const uint8_t> pixels, int width, int height, int rowPitch);
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
        m_contrast = contrast;
        m_saturation = saturation;
    }

    // ─── Paper-white nits (HDR) ───────────────────────────────────────
    void SetPaperWhiteNits(float nits) { m_paperWhiteNits = nits; }

    // ─── Force SDR (diagnostic) ─────────────────────────────────────
    void SetForceSdr(bool force) { m_forceSdr = force; }
    bool IsForceSdr() const { return m_forceSdr; }

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

    // ─── Audio spectrum ────────────────────────────────────────────────
    /// Updates the audio spectrum constant buffer (32 bands, 0..1).
    void UpdateAudioSpectrum(const std::array<float, 32>& spectrum);

    // ─── Frame Rendering ────────────────────────────────────────────────
    /// Opens a frame: waits for a free frame context (P0-4) and resets
    /// the command list against its allocator. Returns false when the
    /// GPU did not release a context in time — the caller must skip
    /// RenderFrame/EndFrame for this pass (the device is flagged removed
    /// so recovery runs). Holds m_renderMutex until EndFrame.
    bool BeginFrame();
    void RenderFrame();
    void EndFrame();
    void SetClearColor(float r, float g, float b);

    bool IsInitialized()   const { return m_initialized; }
    bool IsDeviceRemoved() const { return m_deviceRemoved; }

    // ─── Occlusion-park signal (vendor-neutral) ────────────────────────
    // Whether this swapchain's last present was dropped by DWM as fully
    // occluded. Render-thread only (written in EndFrame, read in the
    // engine loop on the same thread).
    bool IsPresentOccluded() const { return m_occlusion.IsOccluded(); }
    uint32_t ConsecutiveOccludedPresents() const {
        return m_occlusion.ConsecutiveOccluded();
    }

    // ─── Telemetry counters (for the memory-retention diagnostics) ─────
    uint64_t GetTextureBytes()        const { return m_textureBytes; }
    uint64_t GetUploadBytes()         const { return m_uploadBytes; }
    uint64_t GetTextureRecreations()  const { return m_textureRecreations; }
    uint64_t GetUploadCount()         const { return m_uploads; }
    uint64_t GetAllocatorsPerUpload() const { return m_allocatorsPerUpload; }

    // ─── Access to shared graphics primitives ──────────────────────────
    graphics::Device&       GetDevice()       { return m_device; }
    const graphics::Device& GetDevice() const { return m_device; }
    graphics::CommandQueue&       GetCommandQueue()       { return ActiveQueue(); }
    const graphics::CommandQueue& GetCommandQueue() const { return ActiveQueue(); }

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

    /// Syncs m_isHdrOutput with the live swapchain state and creates the
    /// HDR PSO on demand (P0-2 / R16). Caller holds m_renderMutex.
    void SyncHdrStateLocked();

    // ─── Color pipeline ─────────────────────────────────────────────────
    /// Fills the root-constant payload (b0) from the active frame's
    /// colorimetry plus display settings.
    void BuildColorConstants(float cb[15], bool hdrOutput) const;

    // ─── Texture Upload ────────────────────────────────────────────────
    bool UploadTextureToGPU(const TextureData& data);

    // ─── Fill Mode Viewport ────────────────────────────────────────────
    void ComputeFillViewport(D3D12_VIEWPORT& vp, D3D12_RECT& scissor) const;

    // ─── Shared primitives ─────────────────────────────────────────────
    graphics::Device       m_device;
    graphics::SwapChain    m_swapChain;
    graphics::CommandQueue m_commandQueue;
    graphics::CommandQueue* m_sharedQueue = nullptr; // non-owning; set when shared
    graphics::ResourceStateTracker m_stateTracker;

    // ─── Per-frame command state (P0-4 / R17) ──────────────────────────
    // One allocator per in-flight frame: an allocator is Reset() only
    // after its fence value completed (see FrameContextPolicy). The single
    // command list is re-associated with the current context's allocator
    // in BeginFrame.
    struct FrameContext {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        UINT64 fenceValue = 0;  // queue fence signaled after this ctx's work
    };
    FrameContext m_frameContexts[graphics::kFrameContextCount];
    UINT64 m_frameNumber = 0;
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
    bool m_isStaticTexture = false;  // true when rendering a static RGBA texture
    int  m_texWidth  = 0;
    int  m_texHeight = 0;

    // ─── Fullscreen quad UV crop ───────────────────────────────────────
    // Crops driver-aligned pool padding (e.g. 1152-high texture for 1080p
    // content) out of the sampled region. Applied by rewriting the 4 quad
    // UVs (upload heap) only when the video/static state changes — never
    // per frame. (1,1) = sample the whole texture.
    void EnsureVertexUVs(float sx, float sy);
    float m_appliedUvX = 1.0f;
    float m_appliedUvY = 1.0f;

    // ─── Video (shared-handle) state ───────────────────────────────────
    // Opened D3D12 views of the decoder's BGRA pool slots. One SRV per
    // slot, laid out in the SRV heap after the static texture (slots 1..8).
    struct VideoSlotResource {
        HANDLE sharedHandle = nullptr;
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        D3D12_GPU_DESCRIPTOR_HANDLE srvGpu{};
        // Decoder pool textures are driver-aligned (e.g. 1920x1152 for a
        // 1920x1080 frame); the active picture is smaller. The shader crops
        // via uvScale so zeroed padding rows never present (green bar).
        UINT texWidth = 0;
        UINT texHeight = 0;
    };
    static constexpr UINT kMaxVideoSlots = 8;
    std::array<VideoSlotResource, kMaxVideoSlots> m_videoSlots;
    UINT   m_videoSlotCount = 0;
    HANDLE m_activeVideoHandle = nullptr;
    bool   m_hasVideoFrame = false;
    int    m_videoWidth  = 0;
    int    m_videoHeight = 0;
    // Active-picture UV crop (active / texture dims, 1.0 = exact fit).
    // Recomputed per DrawVideoFrame from the bound slot; (1,1) whenever
    // no video frame is bound (static textures are never padded).
    float  m_videoUvScaleX = 1.0f;
    float  m_videoUvScaleY = 1.0f;

    // ─── Color pipeline state ──────────────────────────────────────────
    VideoColorInfo m_activeColorInfo;   // colorimetry of the bound frame
    bool m_isHdrOutput = false;         // swap chain is R16F + PQ/BT.2020
    bool m_forceSdr = false;            // --force-sdr: override hdrOutput to 0
    float m_displayPeakNits = 80.0f;    // tone-mapping target (display or
                                        // SDR reference white in nits)
    float m_inputFormat = 0.0f;         // shader input format (0=NV12, 1=RGBA16, 2=P010)
    float m_debugMode = 0.0f;           // shader debug (0=normal, 1=Y-only, 2=UV-only)

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
    float m_paperWhiteNits = 203.0f;  // SDR reference white on HDR displays

    // ─── Dimensions & state ───────────────────────────────────────────
    int   m_width = 0;
    int   m_height = 0;
    float m_clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    bool  m_initialized  = false;
    bool  m_deviceRemoved = false;

    // Per-swapchain present-occlusion tracker (occlusion park). Written in
    // EndFrame, read by the engine on the same render thread.
    rex::wallpaper::OcclusionTracker m_occlusion;

    // ─── Thread safety ────────────────────────────────────────────────
    mutable std::mutex m_renderMutex;
    // R1: same-thread reentry detector for the frame window. BeginFrame
    // arms it (after locking), EndFrame disarms it (before unlocking);
    // LoadTexture refuses with an error instead of self-deadlocking when
    // called on the frame owner thread inside a frame.
    FrameLockPolicy m_frameLock;

    /// Returns the command queue used for GPU submissions: the shared
    /// queue when compositing, or the local queue for the main renderer.
    graphics::CommandQueue& ActiveQueue() {
        return m_sharedQueue ? *m_sharedQueue : m_commandQueue;
    }
    const graphics::CommandQueue& ActiveQueue() const {
        return m_sharedQueue ? *m_sharedQueue : m_commandQueue;
    }

    // ─── Telemetry ─────────────────────────────────────────────────────
    std::atomic<uint64_t> m_textureBytes{0};
    std::atomic<uint64_t> m_uploadBytes{0};
    std::atomic<uint64_t> m_textureRecreations{0};
    std::atomic<uint64_t> m_uploads{0};
    // Cumulative command allocators/lists created inside upload paths.
    // Flatlines after warm-up (P5 ring); growth during playback = regression.
    std::atomic<uint64_t> m_allocatorsPerUpload{0};

    // ─── Persistent copy/upload contexts (P5) ─────────────────────────
    // Ring of allocator+list pairs reused by the software-copy and texture
    // upload paths. Created once in Initialize(); steady-state uploads
    // perform ZERO command-buffer allocations. Each path executes then
    // WaitForGPUs, so entries are always GPU-idle before reuse.
    static constexpr UINT kCopyContextCount = 4;
    struct CopyContext {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    };
    CopyContext m_copyContexts[kCopyContextCount]{};
    UINT        m_copyContextNext = 0;
    /// Fetches the next ring slot, creating it lazily (warm-up/device-loss
    /// recovery only). List comes back closed-and-ready for Reset().
    shared::Result<void> AcquireCopyContext(
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator>& allocatorOut,
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>& listOut);

    // ─── Vertex layout ────────────────────────────────────────────────
    struct Vertex { float x, y, u, v; };

    // ─── Audio spectrum constant buffer ────────────────────────────────
    Microsoft::WRL::ComPtr<ID3D12Resource> m_audioCB;
    std::array<float, 32>* m_mappedSpectrum = nullptr;
    std::array<float, 32> m_currentSpectrum{};
};

} // namespace rex::wallpaper
