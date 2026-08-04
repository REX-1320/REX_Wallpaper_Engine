#include "WidgetHost.hpp"
#include "WidgetRegistry.hpp"
#include <dwmapi.h>



namespace rex::widgets {

ATOM WidgetHost::s_widgetClassAtom = 0;

ATOM WidgetHost::RegisterWidgetClass() {
    if (s_widgetClassAtom) return s_widgetClassAtom;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"REXWidgetWindow";
    wc.hbrBackground = nullptr;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.style = CS_HREDRAW | CS_VREDRAW;

    s_widgetClassAtom = RegisterClassExW(&wc);
    return s_widgetClassAtom;
}

WidgetHost::WidgetHost(std::shared_ptr<WidgetRegistry> registry)
    : m_registry(registry) {
    RegisterWidgetClass();
}

WidgetHost::~WidgetHost() {
    for (auto& [id, w] : m_windows) {
        if (w.hwnd) DestroyWindow(w.hwnd);
    }
    m_windows.clear();
}

bool WidgetHost::createWindow(const WidgetInstance& instance, const WidgetDefinition& def) {
    if (m_windows.count(instance.id)) return false;

    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        L"REXWidgetWindow",
        L"",
        WS_POPUP,
        instance.x, instance.y, instance.width, instance.height,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

    if (!hwnd) return false;

    SetLayeredWindowAttributes(hwnd, 0, static_cast<BYTE>(instance.opacity * 255.0f), LWA_ALPHA);

    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOZORDER | SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    {
        MARGINS margins = {-1};
        DwmExtendFrameIntoClientArea(hwnd, &margins);
    }

    WidgetWindow ww;
    ww.hwnd = hwnd;
    ww.instance = instance;
    m_windows[instance.id] = ww;

    ShowWindow(hwnd, SW_SHOW);
    return true;
}

void WidgetHost::destroyWindow(const std::string& instanceId) {
    auto it = m_windows.find(instanceId);
    if (it != m_windows.end()) {
        if (it->second.hwnd) {
            DestroyWindow(it->second.hwnd);
        }
        m_windows.erase(it);
    }
}

void WidgetHost::moveWindow(const std::string& instanceId, int x, int y) {
    auto it = m_windows.find(instanceId);
    if (it != m_windows.end() && it->second.hwnd) {
        SetWindowPos(it->second.hwnd, nullptr, x, y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

void WidgetHost::setVisible(const std::string& instanceId, bool visible) {
    auto it = m_windows.find(instanceId);
    if (it != m_windows.end() && it->second.hwnd) {
        ShowWindow(it->second.hwnd, visible ? SW_SHOW : SW_HIDE);
    }
}

LRESULT CALLBACK WidgetHost::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_DESTROY:
        return 0;
    case WM_ERASEBKGND:
        return 1;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

} // namespace rex::widgets
