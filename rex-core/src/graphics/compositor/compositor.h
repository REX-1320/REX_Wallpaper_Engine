#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl.h>
#include <vector>
#include <unordered_map>
#include <cstdint>

using Microsoft::WRL::ComPtr;

struct Layer {
    uint64_t id;
    uint32_t kind;
    ComPtr<ID3D12Resource> texture;
    float bounds[4];
    float opacity;
    int zOrder;
    bool visible;
};

class Compositor {
public:
    Compositor();
    ~Compositor();

    bool Initialize(HWND targetWindow);
    void Shutdown();
    void RenderFrame();

    void SubmitLayer(const Layer& layer);
    void RemoveLayer(uint64_t id);
    void SetClearColor(float r, float g, float b);

    bool ImportLayerTexture(uint64_t layerId, HANDLE sharedHandle, int width, int height);
    void RemoveLayerTexture(uint64_t layerId);

    bool SetBackgroundFromFile(const char* filePath);
    bool UpdateLayerPixels(uint64_t layerId, const uint8_t* pixels,
                           int width, int height, int rowPitch);

    float GetViewportWidth()  const { return m_viewportWidth; }
    float GetViewportHeight() const { return m_viewportHeight; }

    static constexpr uint32_t MAX_TEXTURES = 64;

private:
    bool CreateDevice();
    bool CreateRootSignature();
    bool CreatePipelineState();
    bool CreateSwapChain(HWND targetWindow);
    void CreateSrvHeap();
    bool CreateVertexBuffer();
    void PopulateCommandList();

    ComPtr<ID3D12Device> m_device;
    ComPtr<IDXGISwapChain3> m_swapChain;
    ComPtr<ID3D12CommandQueue> m_commandQueue;
    ComPtr<ID3D12CommandAllocator> m_commandAllocator;
    ComPtr<ID3D12GraphicsCommandList> m_commandList;
    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12PipelineState> m_pipelineState;
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    ComPtr<ID3D12DescriptorHeap> m_srvHeap;

    static constexpr int FRAME_COUNT = 3;
    ComPtr<ID3D12Resource> m_renderTargets[FRAME_COUNT];
    UINT m_rtvDescriptorSize = 0;
    UINT m_srvDescriptorSize = 0;
    UINT m_frameIndex = 0;

    ComPtr<ID3D12Resource> m_vertexBuffer;
    ComPtr<ID3D12Resource> m_indexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_vertexBufferView{};
    D3D12_INDEX_BUFFER_VIEW m_indexBufferView{};
    uint32_t m_indexCount = 0;

    struct LayerTexture {
        ComPtr<ID3D12Resource> resource;
        int srvSlot = -1;
        int texWidth = 0;
        int texHeight = 0;
    };
    std::unordered_map<uint64_t, LayerTexture> m_textures;
    std::vector<uint32_t> m_freeSrvSlots;
    uint32_t m_nextSrvSlot = 0;

    std::vector<Layer> m_layers;
    float m_clearColor[4] = { 0.0f, 0.5f, 0.8f, 1.0f };
    float m_viewportWidth = 800.0f;
    float m_viewportHeight = 600.0f;

    // Reusable video frame upload resources
    ComPtr<ID3D12Resource> m_videoUploadBuffer;
    ComPtr<ID3D12CommandAllocator> m_videoCopyAlloc;
    ComPtr<ID3D12GraphicsCommandList> m_videoCopyList;
    ComPtr<ID3D12Fence> m_videoCopyFence;
    HANDLE m_videoCopyFenceEvent = nullptr;
    UINT64 m_videoCopyFenceVal = 0;
    uint32_t m_videoUploadSize = 0;

    bool m_initialized = false;
};
