#pragma once
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl.h>
#include <vector>
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

private:
    bool CreateDevice();
    bool CreateSwapChain(HWND targetWindow);
    void PopulateCommandList();

    ComPtr<ID3D12Device> m_device;
    ComPtr<IDXGISwapChain3> m_swapChain;
    ComPtr<ID3D12CommandQueue> m_commandQueue;
    ComPtr<ID3D12CommandAllocator> m_commandAllocator;
    ComPtr<ID3D12GraphicsCommandList> m_commandList;
    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12PipelineState> m_pipelineState;
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;

    static constexpr int FRAME_COUNT = 3;
    ComPtr<ID3D12Resource> m_renderTargets[FRAME_COUNT];
    UINT m_rtvDescriptorSize = 0;
    UINT m_frameIndex = 0;

    std::vector<Layer> m_layers;
    float m_clearColor[4] = { 0.0f, 0.5f, 0.8f, 1.0f };

    bool m_initialized = false;
};
