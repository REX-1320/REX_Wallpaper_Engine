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

    /// True when the swap chain targets an HDR10 output and presents in
    /// R16G16B16A16_FLOAT + PQ (ST.2084) / BT.2020.
    bool IsHDR() const { return m_isHdr; }
    DXGI_FORMAT GetBackBufferFormat() const { return m_format; }

    /// Display luminance capability (nits) as reported by DXGI_OUTPUT_DESC1.
    /// Valid only when IsHDRActive() (falls back to 0 otherwise).
    float GetDisplayMaxNits() const { return m_displayMaxNits; }
    float GetDisplayMinNits() const { return m_displayMinNits; }
    /// True when HDR10 presentation is actually active on this swap chain.
    bool IsHDRActive() const { return m_hdrActive; }

    /// Force SDR mode: prevents UpdateHdrSelection from switching to R16F/PQ.
    void SetForceSdr(bool force) { m_forceSdr = force; }
    bool IsForceSdr() const { return m_forceSdr; }

private:
    shared::Result<void> CreateRenderTargetViews(ID3D12Device* device);
    /// Queries the output the swap chain is currently on and picks the
    /// back-buffer format + color space (HDR10 vs SDR). No-op if the
    /// selection matches the current one.
    void UpdateHdrSelection();

    Microsoft::WRL::ComPtr<IDXGISwapChain4>     m_swapChain;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource>       m_renderTargets[3];
    D3D12_CPU_DESCRIPTOR_HANDLE                 m_rtvHandles[3];

    HANDLE  m_frameLatencyWaitable = nullptr;
    UINT    m_bufferCount = 3;
    DXGI_FORMAT m_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    bool    m_isHdr = false;
    float   m_displayMaxNits = 0.0f;
    float   m_displayMinNits = 0.0f;
    bool    m_hdrActive = false;
    bool    m_forceSdr = false;
    bool    m_initialized = false;
};

} // namespace rex::graphics
