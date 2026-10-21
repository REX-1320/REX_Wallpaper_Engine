#include "SwapChain.hpp"
#include "graphics/renderer/SdrWhiteLevel.hpp"
#include <shared/Logger.hpp>
#include <exception>
#include <chrono>

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
    if (FAILED(hr) && desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM) {
        // 10-bit SDR is the mpv-parity target, but it is not universal:
        // a driver that refuses R10G10B10A2_UNORM as an SDR back buffer
        // must still get a working wallpaper. Fall back to 8-bit UNORM.
        REX_LOG_WARN("SwapChain",
                     "10-bit back buffer refused (0x{:08X}); using 8-bit UNORM",
                     static_cast<unsigned>(hr));
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        m_format = desc.Format;
        hr = factory->CreateSwapChainForHwnd(
            commandQueue, hwnd, &desc, nullptr, nullptr, &swapChain1);
    }
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
                 m_isHdr ? "R16G16B16A16_FLOAT HDR10"
                        : (m_format == DXGI_FORMAT_R10G10B10A2_UNORM
                               ? "R10G10B10A2_UNORM SDR 10-bit"
                               : "R8G8B8A8_UNORM SDR 8-bit"),
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

    // P1-12: ResizeBuffers drops states not re-passed in Flags — without
    // FRAME_LATENCY_WAITABLE_OBJECT the cached waitable goes dead and
    // WaitForFrame() burns a 1 s timeout per frame (~1 fps stutter). The
    // HDR path's internal resize already passes it; do the same here and
    // re-query the waitable (ResizeBuffers may replace it).
    HRESULT hr = m_swapChain->ResizeBuffers(
        m_bufferCount,
        static_cast<UINT>(std::max(width, 1)),
        static_cast<UINT>(std::max(height, 1)),
        m_format, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);

    if (FAILED(hr)) {
        return shared::Result<void>(shared::ErrorCode::DriverError,
                                    "ResizeBuffers failed");
    }

    if (m_frameLatencyWaitable) {
        CloseHandle(m_frameLatencyWaitable);
        m_frameLatencyWaitable = nullptr;
    }
    Microsoft::WRL::ComPtr<IDXGISwapChain2> swapChain2;
    if (SUCCEEDED(m_swapChain.As(&swapChain2))) {
        swapChain2->SetMaximumFrameLatency(2);
        m_frameLatencyWaitable = swapChain2->GetFrameLatencyWaitableObject();
    }

    // Audit 5.6 #13: SetColorSpace1 can fail (unsupported space on this
    // output) — log instead of silently rendering with the wrong transform.
    const HRESULT csHr = m_swapChain->SetColorSpace1(
        m_isHdr ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
                : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    if (FAILED(csHr)) {
        REX_LOG_WARN("SwapChain", "SetColorSpace1 failed (0x{:08X})", csHr);
    }

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

bool SwapChain::SampleDisplayCadence(DisplayCadenceStats::Sample& out) {
    if (!m_swapChain) return false;
    // No frame spans 64+ refreshes; anything larger is a non-converged stat
    // block, not a measurement.
    //
    // This bound applies to the PER-FRAME DELTA, not to the raw counter:
    // DXGI_FRAME_STATISTICS::PresentRefreshCount is a *running total* of
    // v-blanks since the swap chain was created, so it exceeds 64 within
    // 400 ms at 160 Hz. Gating the raw value (as this function previously did)
    // closed the gate permanently on every machine, which is why the cadence
    // sampler never produced a sample.
    constexpr UINT kMaxPlausibleRefreshes = 64;

    // Convert a cumulative PresentRefreshCount into v-blanks since the last
    // sample. Returns false until the baseline is established (or must be
    // re-established after a DISJOINT event).
    const auto toDelta = [this](UINT cumulative) -> bool {
        if (!m_cadenceBaseValid) {
            m_cadenceBasePresent = cumulative;
            m_cadenceBaseValid = true;
            return false;
        }
        if (cumulative < m_cadenceBasePresent) {
            // Counter went backwards: treat as a discontinuity, re-baseline.
            m_cadenceBasePresent = cumulative;
            return false;
        }
        const UINT delta = cumulative - m_cadenceBasePresent;
        m_cadenceBasePresent = cumulative;
        m_cadenceLastDelta = delta;
        return true;
    };

    // Route 1: IDXGISwapChainMedia. Purpose-built for media apps, and it also
    // exposes CompositionMode (COMPOSED vs OVERLAY) and ApprovedPresentDuration,
    // which the base structure does not.
    Microsoft::WRL::ComPtr<IDXGISwapChainMedia> media;
    if (SUCCEEDED(m_swapChain.As(&media))) {
        DXGI_FRAME_STATISTICS_MEDIA mst{};
        const HRESULT hr = media->GetFrameStatisticsMedia(&mst);
        static int s_mediaLog = 0;
        if (s_mediaLog < 3 && hr != DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) {
            ++s_mediaLog;
            REX_LOG_INFO("SwapChain",
                         "raw GetFrameStatisticsMedia hr=0x{:08x} PresentCount={} "
                         "PresentRefreshCount={} SyncRefreshCount={} Mode={} "
                         "ApprovedPresentDuration={}",
                         static_cast<unsigned long>(hr), mst.PresentCount,
                         mst.PresentRefreshCount, mst.SyncRefreshCount,
                         static_cast<int>(mst.CompositionMode),
                         mst.ApprovedPresentDuration);
        }
        if (SUCCEEDED(hr) && mst.PresentCount > 0 && mst.PresentRefreshCount > 0 &&
            toDelta(mst.PresentRefreshCount) &&
            m_cadenceLastDelta > 0 &&
            m_cadenceLastDelta <= kMaxPlausibleRefreshes) {
            m_lastCompositionMode.store(
                static_cast<int>(mst.CompositionMode), std::memory_order_relaxed);
            out.presentRefreshCount = m_cadenceLastDelta;
            out.syncRefreshCount = mst.SyncRefreshCount;
            out.syncQpc = mst.SyncQPCTime.QuadPart;
            out.valid = true;
            m_cadence.Add(out);
            return true;
        }
    }

    // Route 2: base IDXGISwapChain::GetFrameStatistics.
    //
    // PresentCount is images-reached-monitor; SyncRefreshCount and
    // PresentRefreshCount are v-blank counters. They are different units and
    // are NOT expected to be equal — requiring PresentCount ==
    // SyncRefreshCount was a unit conflation that rejected every sample.
    // Convergence is expressed by PresentRefreshCount == SyncRefreshCount (no
    // v-blank elapsed since the last present/scheduler pair), which does not
    // need to be true for a valid per-frame dwell reading.
    DXGI_FRAME_STATISTICS st{};
    const HRESULT hr = m_swapChain->GetFrameStatistics(&st);
    if (hr == DXGI_ERROR_FRAME_STATISTICS_DISJOINT) {
        // Documented as recoverable ("an event, for example a power cycle,
        // interrupted the gathering of presentation statistics"). Re-baseline
        // instead of abandoning the stream until the swap chain is recreated.
        m_cadenceBaseValid = false;
        return false;
    }
    if (FAILED(hr)) return false;
    if (st.PresentCount == 0) return false;
    if (st.PresentRefreshCount == 0) return false;
    if (!toDelta(st.PresentRefreshCount)) return false;
    if (m_cadenceLastDelta == 0 ||
        m_cadenceLastDelta > kMaxPlausibleRefreshes) {
        return false;
    }
    out.presentRefreshCount = m_cadenceLastDelta;
    out.syncRefreshCount = st.SyncRefreshCount;
    out.syncQpc = st.SyncQPCTime.QuadPart;
    out.valid = true;
    m_cadence.Add(out);
    return true;
}

shared::Result<void> SwapChain::RefreshHdrSelection(ID3D12Device* device) {
    if (!m_swapChain) {
        return shared::Result<void>(shared::ErrorCode::NotInitialized,
                                    "SwapChain not initialized");
    }
    const DXGI_FORMAT before = m_format;
    // May ResizeBuffers(0,0) internally when the format changes; R1: it
    // also drops the old RTVs/back buffers (ResizeBuffers requires it).
    UpdateHdrSelection();
    if (m_format == before && m_rtvHeap) {
        // No format change and the views are intact — nothing to do.
        return shared::Result<void>();
    }
    // Format changed (the views describe the old buffers) or a failed
    // switch attempt dropped them for the retained format — re-fetch either
    // way, otherwise the next frame binds dead back buffers.
    return CreateRenderTargetViews(device);
}

void SwapChain::WaitForFrame() const {
    if (m_frameLatencyWaitable) {
        WaitForSingleObjectEx(m_frameLatencyWaitable, 1000, FALSE);
    }
}

// Phase timing for the waitable wait (see wall_research.md 4.2 erratum: the wait
// IS performed correctly in BeginFrame; this exists to measure how long it
// actually blocks, which is the part that was unverified).
double SwapChain::MeasureWaitForFrameMs() const {
    if (!m_frameLatencyWaitable) return 0.0;
    const auto t0 = std::chrono::steady_clock::now();
    WaitForSingleObjectEx(m_frameLatencyWaitable, 1000, FALSE);
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
}

// ─── Private ───────────────────────────────────────────────────────────────

void SwapChain::UpdateHdrSelection() {
    if (!m_swapChain) return;

    // m_forceSdr selects SDR output — including *reverting* an existing
    // R16F/PQ format. The renderer's failed-HDR-PSO path forces SDR to
    // bring its cached flag and the real buffers back into agreement, so
    // this can no longer early-return while the swapchain is HDR (audit
    // 2026-10-17, #4); when forced and already SDR it still no-ops below.
    bool hdr = false;
    DXGI_OUTPUT_DESC1 desc1 = {};
    if (!m_forceSdr) {
        Microsoft::WRL::ComPtr<IDXGIOutput> output;
        if (FAILED(m_swapChain->GetContainingOutput(&output))) return;
        Microsoft::WRL::ComPtr<IDXGIOutput6> out6;
        if (FAILED(output.As(&out6)) || FAILED(out6->GetDesc1(&desc1))) return;

        // The output's device name (e.g. "\\.\DISPLAY1"): the OS's SDR white
        // level is looked up per display, so the renderer needs to know which
        // display it is driving.
        m_outputDeviceName = desc1.DeviceName;

        // HDR10-capable outputs advertise the PQ/BT.2020 color space. scRGB
        // (advanced color) is intentionally treated as SDR for now.
        hdr = desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;

        // An HDR DESKTOP is not visible in that colour space: the DWM keeps
        // presenting SDR windows with G22 even when the display is in HDR.
        // Presenting a 10-bit UNORM buffer into an HDR desktop is what made
        // the wallpaper render green/magenta (the R and B channels wrong for
        // every SDR file) - the compositor expects the HDR output path there.
        if (!hdr && QueryDisplayHdrEnabled(m_outputDeviceName)) {
            hdr = true;
            REX_LOG_INFO("SwapChain",
                         "the display is in an HDR mode: using the HDR "
                         "output path (PQ on an FP16 back buffer)");
        }
    }
    DXGI_FORMAT fmt = hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT
                          : (m_sdrTenBitUnsupported
                                 ? DXGI_FORMAT_R8G8B8A8_UNORM
                                 : DXGI_FORMAT_R10G10B10A2_UNORM);
    if (hdr == m_isHdr && fmt == m_format) return;

    // R1: ResizeBuffers fails while back-buffer references are held, so
    // drop the RTVs + back buffers first. Selection state is committed
    // ONLY on success — the failure branch used to claim SDR while the
    // swapchain kept its previous (possibly HDR) format, desyncing the
    // PSO/RTV from the real buffers.
    for (auto& rt : m_renderTargets) rt.Reset();
    m_rtvHeap.Reset();
    m_rtvHandles[0] = m_rtvHandles[1] = m_rtvHandles[2] = {};

    // A format change requires ResizeBuffers (0,0 = keep current size);
    // the color space is a per-swap-chain property.
    HRESULT hr = m_swapChain->ResizeBuffers(m_bufferCount, 0, 0, fmt,
        DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
    if (FAILED(hr) && fmt == DXGI_FORMAT_R10G10B10A2_UNORM) {
        // The driver refuses a 10-bit SDR back buffer: fall back to 8-bit
        // UNORM and remember it, so the next refresh does not retry a
        // format this driver has already rejected.
        REX_LOG_WARN("SwapChain",
                     "10-bit back buffer refused on resize (0x{:08X}); "
                     "falling back to 8-bit UNORM",
                     static_cast<unsigned>(hr));
        m_sdrTenBitUnsupported = true;
        fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
        hr = m_swapChain->ResizeBuffers(m_bufferCount, 0, 0, fmt,
            DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
    }
    if (FAILED(hr)) {
        REX_LOG_WARN("SwapChain", "format change resize failed: {:08x}",
                     static_cast<unsigned long>(hr));
        return;
    }

    m_isHdr = hdr;
    m_format = fmt;
    m_hdrActive = hdr;
    m_displayMaxNits = hdr ? static_cast<float>(desc1.MaxLuminance) : 0.0f;
    m_displayMinNits = hdr ? static_cast<float>(desc1.MinLuminance) : 0.0f;

    // ResizeBuffers may replace the frame-latency waitable (P1-12) — a
    // stale handle waits out a 1 s timeout per frame; re-query it like
    // Resize() does.
    if (m_frameLatencyWaitable) {
        CloseHandle(m_frameLatencyWaitable);
        m_frameLatencyWaitable = nullptr;
    }
    Microsoft::WRL::ComPtr<IDXGISwapChain2> swapChain2;
    if (SUCCEEDED(m_swapChain.As(&swapChain2))) {
        swapChain2->SetMaximumFrameLatency(2);
        m_frameLatencyWaitable = swapChain2->GetFrameLatencyWaitableObject();
    }

    // Audit 5.6 #13: log a failed color-space switch (see Resize()).
    const HRESULT csHr = m_swapChain->SetColorSpace1(
        hdr ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
            : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    if (FAILED(csHr)) {
        REX_LOG_WARN("SwapChain", "SetColorSpace1 failed (0x{:08X})", csHr);
    }
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
