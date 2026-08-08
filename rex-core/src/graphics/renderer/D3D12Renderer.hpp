#pragma once

#include "Device.hpp"
#include "SwapChain.hpp"
#include "CommandQueue.hpp"
#include "DescriptorHeapManager.hpp"
#include "wallpaper/decoder/WICTextureLoader.hpp"

#include <shared/Result.hpp>
#include <rex/wallpaper/IWallpaperEngine.hpp>

#include <mutex>
#include <atomic>
#include <string_view>

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
    void Shutdown();
    void Resize(int width, int height);

    // ─── Texture Loading ────────────────────────────────────────────────
    shared::Result<void> LoadTexture(std::string_view path);
    shared::Result<void> UpdateTexturePixels(
        const uint8_t* pixels, int width, int height, int rowPitch);
    void ClearTexture();
    bool HasTexture() const { return m_hasTexture; }

    // ─── Fill Mode ──────────────────────────────────────────────────────
    void SetFillMode(FillMode mode) { m_fillMode = mode; }
    FillMode GetFillMode() const { return m_fillMode; }

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
    bool CreatePipelineState();
    bool CreateVertexBuffer();

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
    Microsoft::WRL::ComPtr<ID3D12PipelineState>  m_pipelineState;
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

    // ─── Fill mode ─────────────────────────────────────────────────────
    FillMode m_fillMode = FillMode::Fill;

    // ─── Dimensions & state ───────────────────────────────────────────
    int   m_width = 0;
    int   m_height = 0;
    float m_clearColor[4] = {0.1f, 0.1f, 0.2f, 1.0f};
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
