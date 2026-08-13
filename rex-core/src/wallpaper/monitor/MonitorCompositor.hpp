#pragma once

#include "graphics/renderer/D3D12Renderer.hpp"

#include <windows.h>
#include <rex/wallpaper/IWallpaperEngine.hpp>

#include <atomic>
#include <memory>
#include <string>

namespace rex::wallpaper {

// ─── MonitorCompositor ─────────────────────────────────────────────────────
//
// Per-monitor presentation context. Owns a child render window inside the
// wallpaper WorkerW layer (a sibling of the engine's main render window,
// positioned exactly on this monitor's rect) plus a D3D12Renderer that
// ADOPTS the engine's D3D12 device — the same device that created the video
// pool slots' NT handles, so shared-handle frames stay valid on any GPU
// topology.
//
// Window hierarchy (siblings under the wallpaper WorkerW, all below the
// desktop icon layer):
//
//   WorkerW (wallpaper layer)
//   ├── REXRenderWindow      (main, full virtual desktop — engine renderer)
//   └── REXMonitorWindow     (one per monitor, HWND_TOP of the main window)
//
// Compositors sit ABOVE the main render window so their content is not
// occluded on their monitor; the main window only matters for monitors that
// have no compositor (never in practice — one is created per monitor).
//
// Threading: all Present* calls must come from the render thread
// (Engine::RenderLoop) — same convention as the main renderer.

class MonitorCompositor {
public:
    explicit MonitorCompositor(MonitorInfo info);
    ~MonitorCompositor();

    MonitorCompositor(const MonitorCompositor&) = delete;
    MonitorCompositor& operator=(const MonitorCompositor&) = delete;

    /// workerW: the wallpaper WorkerW (parent of the engine's render
    /// window). device: the engine's D3D12 device (adopted by the renderer).
    bool Initialize(ID3D12Device* device, HWND workerW);
    void Shutdown();

    /// Present the current video frame (a decoder pool slot) to this
    /// monitor. The command queue waits on waitFence/fenceValue before
    /// sampling (same contract as D3D12Renderer::DrawVideoFrame). Takes
    /// the real VideoFrame so HDR colorimetry travels with the frame.
    /// Order inside: BeginFrame -> DrawVideoFrame -> RenderFrame -> EndFrame.
    void PresentFrame(const VideoFrame& frame, ID3D12Fence* waitFence);

    /// Load and present a static image (per-monitor copy; the main
    /// renderer's texture is not shareable across devices).
    void PresentStaticTexture(const std::string& path);
    void ClearStaticTexture();

    /// Video adjustments (identity 1.0). Fan-out from the engine so
    /// per-monitor renderers match the main renderer.
    void SetVideoAdjustments(float brightness, float contrast, float saturation);

    /// Rotation/flip transform. Fan-out from the engine so per-monitor
    /// renderers match the main renderer.
    void SetTransform(float rotationDegrees, bool flipH, bool flipV);

    /// Drop GPU video state (scene unload). MUST NOT be called between
    /// BeginFrame()/EndFrame().
    void ClearVideoFrame();

    void SetFillMode(FillMode mode);
    void Resize(int width, int height);
    void SetPosition(int x, int y);

    const MonitorInfo& GetInfo() const { return m_info; }
    bool IsActive() const { return m_active; }
    bool IsDeviceRemoved() const {
        return m_renderer && m_renderer->IsDeviceRemoved();
    }
    uint64_t GetFrameCount() const { return m_frameCount; }

private:
    static ATOM RegisterWindowClass();

    void PresentNow();

    MonitorInfo m_info;
    bool m_active = false;
    HWND m_window = nullptr;
    std::unique_ptr<D3D12Renderer> m_renderer;
    std::atomic<uint64_t> m_frameCount{0};
    static ATOM s_windowClass;
};

} // namespace rex::wallpaper
