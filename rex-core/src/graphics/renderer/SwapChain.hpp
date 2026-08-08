#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <shared/Result.hpp>

#include <cstdint>

namespace rex::graphics {

// ─── SwapChain ──────────────────────────────────────────────────────────────
//
// Manages IDXGISwapChain4 creation, RTV descriptor heap, render target views,
// resize, present, and frame latency waitable object for vsync-aligned pacing.
//
// The swap chain is created against a command queue (ID3D12CommandQueue*)
// passed at construction time.

class SwapChain {
public:
    SwapChain() = default;
    ~SwapChain();

    /// Create the swap chain for the given HWND.
    /// @param commandQueue The command queue used for presenting.
    /// @param hwnd         The target window.
    /// @param width,height Initial dimensions.
    /// @param bufferCount  Number of back buffers (default 3).
    shared::Result<void> Initialize(
        ID3D12CommandQueue* commandQueue,
        ID3D12Device* device,
        HWND hwnd,
        int width,
        int height,
        UINT bufferCount = 3);

    void Shutdown();

    /// Resize buffers (called on WM_SIZE).
    shared::Result<void> Resize(
        ID3D12Device* device, int width, int height);

    /// Present the current frame. Returns the HRESULT from Present.
    /// Caller should check IsDeviceRemoved() if the result is an error.
    HRESULT Present(UINT syncInterval = 1, UINT flags = 0);

    /// Wait for the frame latency waitable object (vsync-aligned pacing).
    /// Returns immediately if the waitable object was not created.
    void WaitForFrame() const;

    /// Get current back buffer index (queried live from the swap chain).
    UINT GetCurrentIndex() const {
        return m_swapChain ? m_swapChain->GetCurrentBackBufferIndex() : 0;
    }

    /// Get the current back buffer resource.
    ID3D12Resource* GetBuffer(UINT index) const {
        return (index < m_bufferCount) ? m_renderTargets[index].Get() : nullptr;
    }

    /// Get the RTV handle for a back buffer.
    D3D12_CPU_DESCRIPTOR_HANDLE GetRTVHandle(UINT index) const {
        return m_rtvHandles[index];
    }

    /// Get the RTV descriptor heap.
    ID3D12DescriptorHeap* GetRTVHeap() const { return m_rtvHeap.Get(); }

    UINT GetBufferCount() const { return m_bufferCount; }
    bool IsValid() const { return m_swapChain != nullptr; }

private:
    shared::Result<void> CreateRenderTargetViews(ID3D12Device* device);

    Microsoft::WRL::ComPtr<IDXGISwapChain4>     m_swapChain;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_renderTargets[3];
    D3D12_CPU_DESCRIPTOR_HANDLE                 m_rtvHandles[3];

    HANDLE  m_frameLatencyWaitable = nullptr;
    UINT    m_bufferCount = 3;
    bool    m_initialized = false;
};

} // namespace rex::graphics
