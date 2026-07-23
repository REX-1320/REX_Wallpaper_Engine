#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <shared/Result.hpp>

#include <mutex>
#include <string>
#include <string_view>

#include "../decoder/WICTextureLoader.hpp"
#include <wallpaper/IWallpaperEngine.hpp>

namespace rex::wallpaper {

class D3D12Renderer {
public:
    D3D12Renderer();
    ~D3D12Renderer();

    shared::Result<void> Initialize(HWND targetWindow, int width, int height);
    void Shutdown();
    void Resize(int width, int height);

    // ─── Texture Loading ────────────────────────────────────────────────────
    shared::Result<void> LoadTexture(std::string_view path);
    /// Update an already-loaded texture with new RGBA8 pixel data.
    /// Used by video playback to update each frame. Pre-allocates on first call.
    shared::Result<void> UpdateTexturePixels(const uint8_t* pixels, int width,
                                             int height, int rowPitch);
    void ClearTexture();
    bool HasTexture() const { return m_hasTexture; }

    // ─── Fill Mode ──────────────────────────────────────────────────────────
    void SetFillMode(FillMode mode) { m_fillMode = mode; }
    FillMode GetFillMode() const { return m_fillMode; }

    // ─── Frame Rendering ────────────────────────────────────────────────────
    void BeginFrame();
    void RenderFrame();
    void EndFrame();

    void SetClearColor(float r, float g, float b);

    bool IsInitialized() const { return m_initialized; }
    bool IsDeviceRemoved() const { return m_deviceRemoved; }

private:
    // ─── D3D12 Initialization ──────────────────────────────────────────────
    bool CreateDevice();
    bool CreateSwapChain(HWND targetWindow);
    bool CreateCommandQueue();
    bool CreateCommandAllocator();
    bool CreateCommandList();
    bool CreateFence();
    bool CreateRenderTargetViews();
    bool CreateRootSignature();
    bool CreatePipelineState();
    bool CreateVertexBuffer();
    bool CreateDescriptorHeaps();

    // ─── GPU Synchronization ──────────────────────────────────────────────
    void WaitForGPU();
    /// Wait for the frame latency waitable object (vsync-aligned frame pacing).
    /// Returns immediately if the waitable object was not created.
    void WaitForFrame();

    // ─── Texture Upload ────────────────────────────────────────────────────
    bool UploadTextureToGPU(const TextureData& data);

    // ─── Fill Mode Viewport ───────────────────────────────────────────────
    void ComputeFillViewport(D3D12_VIEWPORT& vp, D3D12_RECT& scissor) const;

    // ─── D3D12 Objects ──────────────────────────────────────────────────────
    Microsoft::WRL::ComPtr<ID3D12Device>              m_device;
    Microsoft::WRL::ComPtr<IDXGISwapChain4>            m_swapChain;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue>        m_commandQueue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    m_commandAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commandList;
    Microsoft::WRL::ComPtr<ID3D12Fence>               m_fence;
    HANDLE                                            m_fenceEvent = nullptr;
    HANDLE                                            m_frameLatencyWaitable = nullptr;
    UINT64                                            m_fenceValue = 0;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>      m_rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>      m_srvHeap;
    Microsoft::WRL::ComPtr<ID3D12RootSignature>       m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>       m_pipelineState;
    Microsoft::WRL::ComPtr<ID3D12Resource>            m_vertexBuffer;

    Microsoft::WRL::ComPtr<ID3D12Resource>            m_renderTargets[3];
    D3D12_CPU_DESCRIPTOR_HANDLE                      m_rtvHandles[3];

    // ─── Texture ────────────────────────────────────────────────────────────
    Microsoft::WRL::ComPtr<ID3D12Resource>            m_texture;
    Microsoft::WRL::ComPtr<ID3D12Resource>            m_textureUploadBuffer; // pre-allocated for video
    D3D12_GPU_DESCRIPTOR_HANDLE                      m_textureSRVGpuHandle{};
    bool                                              m_hasTexture = false;
    int                                               m_texWidth = 0;
    int                                               m_texHeight = 0;
    D3D12_RESOURCE_STATES                             m_textureState = D3D12_RESOURCE_STATE_COPY_DEST;

    // ─── Fill Mode ──────────────────────────────────────────────────────────
    FillMode m_fillMode = FillMode::Fill;

    // ─── Dimensions & State ────────────────────────────────────────────────
    int    m_width = 0;
    int    m_height = 0;
    UINT   m_frameIndex = 0;
    float  m_clearColor[4] = {0.1f, 0.1f, 0.2f, 1.0f};
    bool   m_initialized = false;
    bool   m_deviceRemoved = false;

    // ─── Thread Safety ──────────────────────────────────────────────────────
    mutable std::mutex m_renderMutex;

    // ─── Vertex Layout ──────────────────────────────────────────────────────
    struct Vertex {
        float x, y;
        float u, v;
    };
};

} // namespace rex::wallpaper
