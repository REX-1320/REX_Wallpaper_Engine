#include "SwapChain.hpp"
#include <shared/Logger.hpp>
#include <exception>

namespace rex::graphics {

SwapChain::~SwapChain() {
    try {
        Shutdown();
    } catch (const std::exception& ex) {
        REX_LOG_ERROR("SwapChain", "~SwapChain exception: {}", ex.what());
    } catch (...) {
        REX_LOG_ERROR("SwapChain", "~SwapChain unknown exception");
    }
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
    desc.Format       = m_format;
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

    // HDR10 vs SDR: pick the back-buffer format + color space from the
    // output the window currently sits on.
    UpdateHdrSelection();
    REX_LOG_INFO("SwapChain", "Back buffer: {} ({})",
                 m_isHdr ? "R16G16B16A16_FLOAT HDR10" : "R8G8B8A8_UNORM SDR",
                 m_isHdr ? "PQ/BT.2020" : "sRGB/BT.709");

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
}

shared::Result<void> SwapChain::Resize(
    ID3D12Device* device, int width, int height) {

    if (!m_swapChain) {
        return shared::Result<void>(shared::ErrorCode::NotInitialized,
                                    "SwapChain not initialized");
    }

    // Release existing RTVs
    for (auto& rt : m_renderTargets) rt.Reset();

    // Re-evaluate HDR selection (the window may have moved to a different
    // output); may change the buffer format.
    UpdateHdrSelection();

    HRESULT hr = m_swapChain->ResizeBuffers(
        m_bufferCount,
        static_cast<UINT>(std::max(width, 1)),
        static_cast<UINT>(std::max(height, 1)),
        m_format, 0);

    if (FAILED(hr)) {
        return shared::Result<void>(shared::ErrorCode::DriverError,
                                    "ResizeBuffers failed");
    }

    m_swapChain->SetColorSpace1(m_isHdr ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
                                        : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);

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

shared::Result<void> SwapChain::RefreshHdrSelection(ID3D12Device* device) {
    if (!m_swapChain) {
        return shared::Result<void>(shared::ErrorCode::NotInitialized,
                                    "SwapChain not initialized");
    }
    const DXGI_FORMAT before = m_format;
    // May ResizeBuffers(0,0) internally when the format changes.
    UpdateHdrSelection();
    if (m_format == before) {
        return shared::Result<void>();
    }
    // The format changed behind the existing RTVs — they describe the old
    // buffers, so re-fetch them.
    return CreateRenderTargetViews(device);
}

void SwapChain::WaitForFrame() const {
    if (m_frameLatencyWaitable) {
        WaitForSingleObjectEx(m_frameLatencyWaitable, 1000, FALSE);
    }
}

// ─── Private ───────────────────────────────────────────────────────────────

void SwapChain::UpdateHdrSelection() {
    if (!m_swapChain) return;
    if (m_forceSdr) return;  // --force-sdr: stay on SDR format

    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    if (FAILED(m_swapChain->GetContainingOutput(&output))) return;
    Microsoft::WRL::ComPtr<IDXGIOutput6> out6;
    DXGI_OUTPUT_DESC1 desc1 = {};
    if (FAILED(output.As(&out6)) || FAILED(out6->GetDesc1(&desc1))) return;

    // HDR10-capable outputs advertise the PQ/BT.2020 color space. scRGB
    // (advanced color) is intentionally treated as SDR for now.
    const bool hdr = desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    const DXGI_FORMAT fmt = hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                : DXGI_FORMAT_R8G8B8A8_UNORM;
    if (hdr == m_isHdr && fmt == m_format) return;

    m_isHdr = hdr;
    m_format = fmt;
    m_hdrActive = hdr;
    m_displayMaxNits = hdr ? static_cast<float>(desc1.MaxLuminance) : 0.0f;
    m_displayMinNits = hdr ? static_cast<float>(desc1.MinLuminance) : 0.0f;

    // A format change requires ResizeBuffers (0,0 = keep current size);
    // the color space is a per-swap-chain property.
    HRESULT hr = m_swapChain->ResizeBuffers(m_bufferCount, 0, 0, fmt,
        DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
    if (FAILED(hr)) {
        REX_LOG_WARN("SwapChain", "format change resize failed: {:08x}",
                     static_cast<unsigned long>(hr));
        m_isHdr = false;
        m_format = DXGI_FORMAT_R8G8B8A8_UNORM;
        m_hdrActive = false;
        m_displayMaxNits = 0.0f;
        m_displayMinNits = 0.0f;
        return;
    }
    m_swapChain->SetColorSpace1(hdr ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
                                    : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    REX_LOG_INFO("SwapChain", "HDR10 output {}", hdr ? "enabled" : "disabled");
}

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
