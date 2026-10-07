#include "wallpaper/monitor/MonitorCompositor.hpp"

#include <shared/Logger.hpp>

namespace rex::wallpaper {

ATOM MonitorCompositor::s_windowClass = 0;

namespace {
constexpr const char* kModule = "MonitorCompositor";
constexpr wchar_t kWindowClassName[] = L"REXMonitorWindow";

LRESULT CALLBACK MonitorWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
} // namespace

MonitorCompositor::MonitorCompositor(MonitorInfo info)
    : m_info(std::move(info)) {
}

MonitorCompositor::~MonitorCompositor() {
    Shutdown();
}

ATOM MonitorCompositor::RegisterWindowClass() {
    if (s_windowClass) return s_windowClass;

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = MonitorWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = kWindowClassName;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    s_windowClass    = RegisterClassExW(&wc);
    return s_windowClass;
}

bool MonitorCompositor::Initialize(ID3D12Device* device, HWND workerW,
                                   graphics::CommandQueue* sharedQueue) {
    if (m_active) return true;
    if (!device || !workerW || !IsWindow(workerW)) {
        REX_LOG_ERROR(kModule, "[{}] invalid device/parent for init",
                      m_info.id.c_str());
        return false;
    }

    if (!RegisterWindowClass()) {
        REX_LOG_ERROR(kModule, "[{}] Failed to register monitor window class (err={})",
                      m_info.id.c_str(), static_cast<long>(GetLastError()));
        return false;
    }

    // Child of the wallpaper WorkerW: sits in the wallpaper layer, below
    // the desktop icon layer, above the engine's main render window.
    m_window = CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        kWindowClassName, L"",
        WS_CHILD | WS_VISIBLE,
        m_info.x, m_info.y, m_info.width, m_info.height,
        workerW, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!m_window) {
        REX_LOG_ERROR(kModule, "[{}] CreateWindowEx failed (err={})",
                      m_info.id.c_str(), static_cast<long>(GetLastError()));
        return false;
    }

    // Above the main render window (sibling in the same WorkerW), so this
    // monitor's content is never occluded by the main window.
    SetWindowPos(m_window, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    m_renderer = std::make_unique<D3D12Renderer>();
    auto result = sharedQueue
        ? m_renderer->Initialize(m_window, m_info.width, m_info.height,
                                 device, sharedQueue)
        : m_renderer->Initialize(m_window, m_info.width, m_info.height, device);
    if (result.IsError()) {
        REX_LOG_ERROR(kModule, "[{}] renderer init failed: {}",
                      m_info.id.c_str(), result.Error().message);
        DestroyWindow(m_window);
        m_window = nullptr;
        m_renderer.reset();
        return false;
    }

    m_active = true;
    REX_LOG_INFO(kModule, "[{}] initialized {}x{} at ({},{})",
                 m_info.id.c_str(), m_info.width, m_info.height,
                 m_info.x, m_info.y);
    return true;
}

void MonitorCompositor::Shutdown() {
    if (m_renderer) {
        m_renderer->Shutdown();
        m_renderer.reset();
    }
    if (m_window) {
        DestroyWindow(m_window);
        m_window = nullptr;
    }
    m_active = false;
}

void MonitorCompositor::PresentFrame(const VideoFrame& frame,
                                     ID3D12Fence* waitFence) {
    if (!m_active || !m_renderer || !frame.valid) return;

    if (!m_renderer->BeginFrame()) return;      // 1. prepare (P0-4: skip on fence timeout)
    m_renderer->DrawVideoFrame(frame, waitFence); // 2. bind shared slot + fence
    m_renderer->RenderFrame();                   // 3. sample + draw
    m_renderer->EndFrame();                      // 4. present
    m_frameCount.fetch_add(1, std::memory_order_relaxed);
}

void MonitorCompositor::PresentStaticTexture(const std::string& path) {
    if (!m_active || !m_renderer) return;

    auto result = m_renderer->LoadTexture(path);
    if (result.IsError()) {
        REX_LOG_WARN(kModule, "[{}] texture load failed: {}",
                     m_info.id.c_str(), result.Error().message);
        return;
    }
    // Static content has no per-frame updates: draw once; the swap chain
    // keeps presenting the last frame.
    if (!m_renderer->BeginFrame()) return;  // P0-4: skip on fence timeout
    m_renderer->RenderFrame();
    m_renderer->EndFrame();
    m_frameCount.fetch_add(1, std::memory_order_relaxed);
}

void MonitorCompositor::ClearStaticTexture() {
    if (m_renderer) {
        m_renderer->ClearTexture();
    }
}

void MonitorCompositor::SetVideoAdjustments(float brightness, float contrast,
                                            float saturation) {
    if (m_renderer) {
        m_renderer->SetVideoAdjustments(brightness, contrast, saturation);
    }
}

void MonitorCompositor::ClearVideoFrame() {
    if (m_renderer) {
        m_renderer->ClearVideoFrame();
    }
}

void MonitorCompositor::SetFillMode(FillMode mode) {
    if (m_renderer) {
        m_renderer->SetFillMode(mode);
    }
}

void MonitorCompositor::SetTransform(float rotationDegrees, bool flipH,
                                     bool flipV) {
    if (m_renderer) {
        m_renderer->SetRotation(rotationDegrees);
        m_renderer->SetFlip(flipH, flipV);
    }
}

void MonitorCompositor::Resize(int width, int height) {
    if (!m_active || !m_renderer) return;

    m_info.width  = width;
    m_info.height = height;
    if (m_window) {
        MoveWindow(m_window, m_info.x, m_info.y, width, height, TRUE);
    }
    m_renderer->Resize(width, height);
}

void MonitorCompositor::SetPosition(int x, int y) {
    if (!m_active || !m_window) return;

    m_info.x = x;
    m_info.y = y;
    MoveWindow(m_window, x, y, m_info.width, m_info.height, TRUE);
}

} // namespace rex::wallpaper