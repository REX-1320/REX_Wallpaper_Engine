#include "SwapChain.hpp"
#include <shared/Logger.hpp>

namespace rex::graphics {

SwapChain::~SwapChain() {
    Shutdown();
}

shared::Result<void> SwapChain::Initialize(
    ID3D12CommandQueue* commandQueue,
    ID3D12Device* device,
    HWND hwnd,
    int width, int height,
    UINT bufferCount) {

    if (m_initialized) {
        return shared::Result<void>(shared::ErrorCode::AlreadyInitialized,
                                    "SwapChain already initialized");
    }

    m_bufferCount = bufferCount;

    Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        return shared::Result<void>(shared::ErrorCode::DriverError,
                                    "CreateDXGIFactory2 failed");
    }

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.BufferCount  = bufferCount;
    desc.Width        = static_cast<UINT>(std::max(width, 1));
    desc.Height       = static_cast<UINT>(std::max(height, 1));
    desc.Format       = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage  = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.SwapEffect   = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.SampleDesc.Count = 1;
    desc.Flags        = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain1;
    hr = factory->CreateSwapChainForHwnd(
        commandQueue, hwnd, &desc, nullptr, nullptr, &swapChain1);
    if (FAILED(hr)) {
        return shared::Result<void>(shared::ErrorCode::DriverError,
                                    "CreateSwapChainForHwnd failed");
    }

    hr = swapChain1.As(&m_swapChain);
    if (FAILED(hr)) {
        return shared::Result<void>(shared::ErrorCode::DriverError,
                                    "Failed to get IDXGISwapChain4");
    }

    // Set max frame latency to 2 for smooth vsync-aligned pacing
    Microsoft::WRL::ComPtr<IDXGISwapChain2> swapChain2;
    if (SUCCEEDED(swapChain1.As(&swapChain2))) {
        swapChain2->SetMaximumFrameLatency(2);
        m_frameLatencyWaitable = swapChain2->GetFrameLatencyWaitableObject();
        REX_LOG_INFO("SwapChain", "Frame latency waitable object created");
    }

    // Disable automatic Alt+Enter handling
    Microsoft::WRL::ComPtr<IDXGIFactory> parentFactory;
    if (SUCCEEDED(m_swapChain->GetParent(IID_PPV_ARGS(&parentFactory)))) {
        parentFactory->MakeWindowAssociation(
            hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    }

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    // Create RTV heap and render target views
    auto rtvResult = CreateRenderTargetViews(device);
    if (rtvResult.IsError()) {
        return rtvResult;
    }

    m_initialized = true;
    REX_LOG_INFO("SwapChain", "Created: {}x{}x{}", width, height, bufferCount);
    return shared::Result<void>();
}

void SwapChain::Shutdown() {
    if (!m_initialized) return;

    for (auto& rt : m_renderTargets) rt.Reset();
    m_rtvHandles[0] = m_rtvHandles[1] = m_rtvHandles[2] = {};
    m_rtvHeap.Reset();
    m_swapChain.Reset();

    if (m_frameLatencyWaitable) {
        CloseHandle(m_frameLatencyWaitable);
        m_frameLatencyWaitable = nullptr;
    }

    m_initialized = false;
    REX_LOG_INFO("SwapChain", "Shutdown");
}

shared::Result<void> SwapChain::Resize(
    ID3D12Device* device, int width, int height) {

    if (!m_swapChain) {
        return shared::Result<void>(shared::ErrorCode::NotInitialized,
                                    "SwapChain not initialized");
    }

    // Release existing RTVs
    for (auto& rt : m_renderTargets) rt.Reset();

    HRESULT hr = m_swapChain->ResizeBuffers(
        m_bufferCount,
        static_cast<UINT>(std::max(width, 1)),
        static_cast<UINT>(std::max(height, 1)),
        DXGI_FORMAT_R8G8B8A8_UNORM, 0);

    if (FAILED(hr)) {
        return shared::Result<void>(shared::ErrorCode::DriverError,
                                    "ResizeBuffers failed");
    }

    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

    auto rtvResult = CreateRenderTargetViews(device);
    if (rtvResult.IsError()) {
        return rtvResult;
    }

    REX_LOG_INFO("SwapChain", "Resized to {}x{}", width, height);
    return shared::Result<void>();
}

HRESULT SwapChain::Present(UINT syncInterval, UINT flags) {
    return m_swapChain->Present(syncInterval, flags);
}

void SwapChain::WaitForFrame() const {
    if (m_frameLatencyWaitable) {
        WaitForSingleObjectEx(m_frameLatencyWaitable, 1000, FALSE);
    }
}

// ─── Private ───────────────────────────────────────────────────────────────

shared::Result<void> SwapChain::CreateRenderTargetViews(ID3D12Device* device) {
    UINT rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // (Re)create RTV heap
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
    rtvDesc.NumDescriptors = m_bufferCount;
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;

    m_rtvHeap.Reset();
    HRESULT hr = device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m_rtvHeap));
    if (FAILED(hr)) {
        return shared::Result<void>(shared::ErrorCode::OutOfMemory,
                                    "CreateDescriptorHeap (RTV) failed");
    }

    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();

    for (UINT i = 0; i < m_bufferCount; ++i) {
        m_renderTargets[i].Reset();
        hr = m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_renderTargets[i]));
        if (FAILED(hr)) {
            return shared::Result<void>(shared::ErrorCode::DriverError,
                                        "GetBuffer failed for slot " + std::to_string(i));
        }
        m_rtvHandles[i] = handle;
        device->CreateRenderTargetView(m_renderTargets[i].Get(), nullptr, handle);
        handle.ptr += rtvDescriptorSize;
    }

    return shared::Result<void>();
}

} // namespace rex::graphics
