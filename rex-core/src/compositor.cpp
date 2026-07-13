#include "compositor.h"
#include <cstdio>
#include <d3dcompiler.h>

Compositor::Compositor() = default;
Compositor::~Compositor() { Shutdown(); }

bool Compositor::Initialize(HWND targetWindow) {
    if (!CreateDevice()) {
        printf("[REX] Compositor: Failed to create D3D12 device\n");
        return false;
    }

    if (!CreateSwapChain(targetWindow)) {
        printf("[REX] Compositor: Failed to create swap chain\n");
        return false;
    }

    m_initialized = true;
    printf("[REX] Compositor initialized (D3D12)\n");
    return true;
}

void Compositor::Shutdown() {
    m_initialized = false;
    for (int i = 0; i < FRAME_COUNT; i++) {
        m_renderTargets[i].Reset();
    }
}

bool Compositor::CreateDevice() {
    ComPtr<ID3D12Debug> debugController;
    D3D12GetDebugInterface(IID_PPV_ARGS(&debugController));
    if (debugController) {
        debugController->EnableDebugLayer();
    }

    ComPtr<IDXGIFactory4> factory;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0,
            IID_PPV_ARGS(&m_device)))) {
            break;
        }
    }

    if (!m_device) return false;

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue));

    m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&m_commandAllocator));

    m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_commandAllocator.Get(), nullptr, IID_PPV_ARGS(&m_commandList));

    m_commandList->Close();

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = FRAME_COUNT;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap));
    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    return true;
}

bool Compositor::CreateSwapChain(HWND targetWindow) {
    ComPtr<IDXGIFactory4> factory;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));

    RECT clientRect;
    GetClientRect(targetWindow, &clientRect);
    UINT width = clientRect.right - clientRect.left;
    UINT height = clientRect.bottom - clientRect.top;

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = FRAME_COUNT;
    swapChainDesc.Width = max(width, 1);
    swapChainDesc.Height = max(height, 1);
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> tempSwapChain;
    factory->CreateSwapChainForHwnd(m_commandQueue.Get(), targetWindow,
        &swapChainDesc, nullptr, nullptr, &tempSwapChain);

    tempSwapChain.As(&m_swapChain);
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    for (int i = 0; i < FRAME_COUNT; i++) {
        m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_renderTargets[i]));
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
            m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtvHandle.ptr += i * m_rtvDescriptorSize;
        m_device->CreateRenderTargetView(m_renderTargets[i].Get(), nullptr, rtvHandle);
    }

    return true;
}

void Compositor::RenderFrame() {
    if (!m_initialized) return;

    m_commandAllocator->Reset();
    m_commandList->Reset(m_commandAllocator.Get(), m_pipelineState.Get());

    PopulateCommandList();
    m_commandList->Close();

    ID3D12CommandList* commandLists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, commandLists);

    m_swapChain->Present(1, 0);
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}

void Compositor::PopulateCommandList() {
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += m_frameIndex * m_rtvDescriptorSize;

    m_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
    m_commandList->ClearRenderTargetView(rtvHandle, m_clearColor, 0, nullptr);

    D3D12_VIEWPORT viewport = { 0.0f, 0.0f, 800.0f, 600.0f, 0.0f, 1.0f };
    m_commandList->RSSetViewports(1, &viewport);

    D3D12_RECT scissorRect = { 0, 0, 800, 600 };
    m_commandList->RSSetScissorRects(1, &scissorRect);
}

void Compositor::SubmitLayer(const Layer& layer) {
    auto it = std::find_if(m_layers.begin(), m_layers.end(),
        [&](const Layer& l) { return l.id == layer.id; });
    if (it != m_layers.end()) {
        *it = layer;
    } else {
        m_layers.push_back(layer);
    }
}

void Compositor::RemoveLayer(uint64_t id) {
    std::erase_if(m_layers, [id](const Layer& l) { return l.id == id; });
}

void Compositor::SetClearColor(float r, float g, float b) {
    m_clearColor[0] = r;
    m_clearColor[1] = g;
    m_clearColor[2] = b;
    m_clearColor[3] = 1.0f;
}
