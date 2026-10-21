#pragma once
#include "graphics/renderer/DisplayCadenceStats.hpp"

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

    /// Re-evaluate the HDR selection outside a resize (same-resolution OS
    /// HDR toggle, window moved outputs). Recreates the RTVs when the
    /// back-buffer format changed. No-op when the selection is unchanged.
    shared::Result<void> RefreshHdrSelection(ID3D12Device* device);

    /// Present the current frame. Returns the HRESULT from Present.
    /// Caller should check IsDeviceRemoved() if the result is an error.
    HRESULT Present(UINT syncInterval = 1, UINT flags = 0);

    /// Wait for the frame latency waitable object (vsync-aligned pacing).
    /// Returns immediately if the waitable object was not created.
    void WaitForFrame() const;

    /// Same wait, but returns how long it blocked (ms). Used by the phase
    /// telemetry to attribute stalls; behaviourally identical to
    /// WaitForFrame().
    double MeasureWaitForFrameMs() const;

    /// Display-cadence samples from DXGI itself: how many refreshes the last
    /// presented frame occupied, and when it synced. This is the only clock in
    /// REX that reflects what the panel actually showed rather than what the
    /// render thread submitted. Returns false when the swap chain cannot supply
    /// statistics (older runtime, or a present that has not synced yet).
    bool SampleDisplayCadence(DisplayCadenceStats::Sample& out);

    const DisplayCadenceStats& CadenceStats() const { return m_cadence; }
    /// DXGI composition mode for the last sampled present: -1 unknown,
    /// 0 DXGI_FRAME_PRESENTATION_MODE_COMPOSED, 1 ..._OVERLAY. Answers from
    /// the API whether this wallpaper surface gets its own plane.
    int CompositionMode() const {
        return m_lastCompositionMode.load(std::memory_order_relaxed);
    }

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
    /// The DXGI output the window sits on ("\\.\DISPLAY1"), empty until the
    /// HDR selection has run. Used to look up per-display settings (the OS's
    /// SDR white level).
    const std::wstring& GetOutputDeviceName() const { return m_outputDeviceName; }

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
    // SDR back buffer: 10-bit UNORM (R10G10B10A2) - the same format mpv's
    // window uses (measured on mpv 0.41.0: "swapchain R10G10B10A2_UNORM,
    // color space RGB_FULL_G22_NONE_P709"). 8-bit UNORM truncates the
    // gradient into visible steps (the banding TODO); 10-bit gives 1024
    // levels per channel and the dither in the pixel shader removes the
    // last of it. Falls back to 8-bit if the driver refuses the format
    // (see SwapChain::Initialize / UpdateHdrSelection).
    DXGI_FORMAT m_format = DXGI_FORMAT_R10G10B10A2_UNORM;
    /// The DXGI output the window sits on ("\\.\DISPLAY1").
    std::wstring m_outputDeviceName;
    /// Sticky: the driver rejected a 10-bit SDR back buffer, so every
    /// later refresh uses 8-bit UNORM instead of retrying.
    bool m_sdrTenBitUnsupported = false;
    bool    m_isHdr = false;
    DisplayCadenceStats m_cadence{};
    // DXGI reports PresentRefreshCount as a running total since swap-chain
    // creation; these hold the baseline so SampleDisplayCadence can publish a
    // true per-frame dwell delta (and re-baseline after a DISJOINT event).
    UINT   m_cadenceBasePresent = 0;
    UINT   m_cadenceLastDelta   = 0;
    bool   m_cadenceBaseValid   = false;
    std::atomic<int> m_lastCompositionMode{-1};  // -1 unknown, 0 COMPOSED, 1 OVERLAY
    float   m_displayMaxNits = 0.0f;
    float   m_displayMinNits = 0.0f;
    bool    m_hdrActive = false;
    bool    m_forceSdr = false;
    bool    m_initialized = false;
};

} // namespace rex::graphics
