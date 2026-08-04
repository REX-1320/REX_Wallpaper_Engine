#include "shell/ShellWindow.hpp"
#include "shell/TopBar.h"
#include "shell/Sidebar.h"
#include "shell/WorkspaceHost.h"
#include "shell/InspectorPanel.h"
#include "shell/LiquidGlassRenderer.h"
#include "shell/NotificationCenter.h"
#include "shell/LiquidGlassTokens.hpp"
#include <d2d1effects.h>
#include <dwmapi.h>
#include <chrono>
#include <thread>

namespace rex::shell {

struct LayoutRects {
    D2D1_RECT_F topBar;
    D2D1_RECT_F sidebar;
    D2D1_RECT_F workspace;
    D2D1_RECT_F inspector;
    D2D1_RECT_F statusBar;
};

ShellWindow::ShellWindow(
    std::shared_ptr<rex::ui::IThemeService> themeService,
    std::shared_ptr<rex::search::ISearchService> searchService,
    std::shared_ptr<rex::command::ICommandService> commandService,
    std::shared_ptr<rex::notifications::INotificationService> notificationService,
    std::shared_ptr<rex::wallpaper::IWallpaperEngine> wallpaperEngine,
    std::shared_ptr<rex::settings::ISettingsService> settingsService,
    std::shared_ptr<rex::widgets::IWidgetService> widgetService,
    std::shared_ptr<rex::personalization::IPersonalizationService> personalizationService,
    std::shared_ptr<rex::modules::ConfigurationStore> configStore,
    std::shared_ptr<rex::modules::EventBus> eventBus)
    : m_themeService(themeService), m_searchService(searchService),
      m_commandService(commandService), m_notificationService(notificationService),
      m_wallpaperEngine(wallpaperEngine), m_settingsService(settingsService),
      m_widgetService(widgetService), m_personalizationService(personalizationService),
      m_configStore(configStore), m_eventBus(eventBus)
{
    InitializeWindow();
    InitializeDComp();
    CreateSurfaces();

    LayoutRects layout;
    ComputeLayout(layout, m_width, m_height);

    m_topBar = std::make_unique<TopBar>();
    m_topBar->Init(m_d2dContext.Get(), m_dwriteFactory.Get(), m_glowStyle);
    m_topBar->SetBounds(layout.topBar.left, layout.topBar.top,
                        layout.topBar.right - layout.topBar.left,
                        layout.topBar.bottom - layout.topBar.top);

    m_sidebar = std::make_unique<Sidebar>();
    m_sidebar->Init(m_d2dContext.Get(), m_dwriteFactory.Get(), m_glowStyle);
    m_sidebar->SetBounds(layout.sidebar.left, layout.sidebar.top,
                         layout.sidebar.right - layout.sidebar.left,
                         layout.sidebar.bottom - layout.sidebar.top);

    m_workspace = std::make_unique<WorkspaceHost>();
    m_workspace->Init(m_d2dContext.Get(), m_dwriteFactory.Get(), m_glowStyle);
    m_workspace->SetBounds(layout.workspace.left, layout.workspace.top,
                           layout.workspace.right - layout.workspace.left,
                           layout.workspace.bottom - layout.workspace.top);

    m_inspector = std::make_unique<InspectorPanel>();
    m_inspector->Init(m_d2dContext.Get(), m_dwriteFactory.Get(), m_glowStyle);
    m_inspector->SetBounds(layout.inspector.left, layout.inspector.top,
                           layout.inspector.right - layout.inspector.left,
                           layout.inspector.bottom - layout.inspector.top);

    m_glass = std::make_unique<LiquidGlassRenderer>();
    m_glass->Init(m_d2dContext.Get());

    m_notifCenter = std::make_unique<NotificationCenter>(
        m_d2dContext.Get(), m_dwriteFactory.Get(), m_glowStyle, m_notificationService);
}

ShellWindow::~ShellWindow() {
    if (m_hwnd) DestroyWindow(m_hwnd);
}

void ShellWindow::Run() {
    MSG msg = {};
    auto lastTime = std::chrono::steady_clock::now();
    while (m_running) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { m_running = false; break; }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;
        dt = (std::min)(dt, 0.05f);
        HandleInput(dt);
        UpdatePhysics(dt);
        RenderFrame();
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
}

void ShellWindow::InitializeWindow() {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ShellWindow";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    RegisterClassW(&wc);

    RECT desktop;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &desktop, 0);
    m_width = desktop.right - desktop.left;
    m_height = desktop.bottom - desktop.top;

    m_hwnd = CreateWindowExW(
        WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TOPMOST,
        wc.lpszClassName, L"REX",
        WS_POPUP,
        desktop.left, desktop.top, m_width, m_height,
        nullptr, nullptr, wc.hInstance, this);

    if (m_hwnd) {
        ShowWindow(m_hwnd, SW_SHOW);
        UpdateWindow(m_hwnd);
        SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }
}

void ShellWindow::InitializeDComp() {
    HRESULT hr;

    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
        D3D11_SDK_VERSION, &m_d3dDevice, nullptr, nullptr);
    if (FAILED(hr)) return;

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    m_d3dDevice.As(&dxgiDevice);

    hr = D2D1CreateDevice(dxgiDevice.Get(), nullptr, &m_d2dDevice);
    if (FAILED(hr)) return;

    m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &m_d2dContext);
    if (!m_d2dContext) return;

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf()));
    if (FAILED(hr)) return;

    hr = DCompositionCreateDevice(dxgiDevice.Get(),
        __uuidof(IDCompositionDevice),
        reinterpret_cast<void**>(m_dcompDevice.GetAddressOf()));
    if (FAILED(hr) || !m_dcompDevice) return;

    hr = m_dcompDevice->CreateTargetForHwnd(m_hwnd, TRUE, &m_dcompTarget);
    if (FAILED(hr)) return;

    hr = m_dcompDevice->CreateVisual(&m_rootVisual);
    if (FAILED(hr)) return;

    hr = m_dcompTarget->SetRoot(m_rootVisual.Get());
    if (FAILED(hr)) return;

    m_dcompDevice->Commit();
}

void ShellWindow::CreateSurfaces() {
    if (!m_d2dContext) return;

    m_d2dContext->CreateSolidColorBrush(
        D2D1::ColorF(0.08f, 0.08f, 0.12f, 0.75f), &m_glassTintBrush);

    m_d2dContext->CreateEffect(CLSID_D2D1GaussianBlur, &m_gaussianBlur);
    if (m_gaussianBlur) {
        m_gaussianBlur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, 24.0f);
        m_gaussianBlur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE,
            D2D1_BORDER_MODE_HARD);
    }

    m_glowStyle.textColor = D2D1::ColorF(1, 1, 1, 1);
    m_glowStyle.glowColor = D2D1::ColorF(0.4f, 0.6f, 1.0f, 1);
}

void ShellWindow::ComputeLayout(LayoutRects& out, int cw, int ch) {
    float w = (float)cw;
    float h = (float)ch;

    float sidebarW = 260.0f;
    float inspectorW = 320.0f;

    if (w < 1366.0f) {
        sidebarW = 64.0f;
        inspectorW = 0.0f;
    } else if (w <= 1440.0f) {
        sidebarW = 240.0f; inspectorW = 280.0f;
    } else if (w <= 1600.0f) {
        sidebarW = 260.0f; inspectorW = 320.0f;
    } else if (w <= 1920.0f) {
        sidebarW = 260.0f; inspectorW = 320.0f;
    } else {
        sidebarW = 280.0f; inspectorW = 360.0f;
    }

    out.topBar = D2D1::RectF(0, 0, w, 48.0f);
    out.statusBar = D2D1::RectF(0, h - 28.0f, w, h);
    out.sidebar = D2D1::RectF(0, 48.0f, sidebarW, h - 28.0f);
    out.inspector = D2D1::RectF(w - inspectorW, 48.0f, w, h - 28.0f);
    out.workspace = D2D1::RectF(sidebarW, 48.0f, w - inspectorW, h - 28.0f);
}

void ShellWindow::HandleInput(float dt) {
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(m_hwnd, &pt);
    float mx = (float)pt.x, my = (float)pt.y;

    bool inTopBar = my < 48.0f;
    LayoutRects layout;
    ComputeLayout(layout, m_width, m_height);

    if (m_notifCenter && m_notifCenter->IsVisible()) {
        m_notifCenter->HandleHover(mx, my);
        return;
    }

    if (inTopBar && m_topBar) {
        m_topBar->HandleHover(mx, my);
    } else if (mx >= layout.sidebar.left && mx <= layout.sidebar.right && m_sidebar) {
        m_sidebar->HandleHover(mx, my);
        m_sidebar->SetHoveredItem(mx, my);
    }
}

void ShellWindow::UpdatePhysics(float dt) {
    if (m_topBar) m_topBar->Update(dt);
    if (m_sidebar) m_sidebar->Update(dt);
    if (m_workspace) m_workspace->Update(dt);
    if (m_inspector) m_inspector->Update(dt);
    if (m_notifCenter) m_notifCenter->Update(dt);
}

void ShellWindow::RenderFrame() {
    if (!m_d2dContext || !m_dcompDevice) return;

    LayoutRects layout;
    ComputeLayout(layout, m_width, m_height);

    m_d2dContext->BeginDraw();
    m_d2dContext->Clear(D2D1::ColorF(0, 0, 0, 0));

    if (m_glass) {
        m_glass->RenderGlass(m_d2dContext.Get(), &layout);
    }

    if (m_topBar) m_topBar->Render(m_d2dContext.Get());
    if (m_sidebar) m_sidebar->Render(m_d2dContext.Get());
    if (m_workspace) m_workspace->Render(m_d2dContext.Get());
    if (m_inspector) m_inspector->Render(m_d2dContext.Get());

    if (m_notifCenter && m_notifCenter->IsVisible())
        m_notifCenter->Render(m_d2dContext.Get());

    HRESULT hr = m_d2dContext->EndDraw();
    if (FAILED(hr)) return;

    m_dcompDevice->Commit();
}

LRESULT CALLBACK ShellWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ShellWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        CREATESTRUCT* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        self = static_cast<ShellWindow*>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    self = reinterpret_cast<ShellWindow*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (self) return self->HandleMessage(hwnd, msg, wParam, lParam);
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

LRESULT ShellWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_SIZE: {
            m_width = LOWORD(lParam);
            m_height = HIWORD(lParam);
            LayoutRects layout;
            ComputeLayout(layout, m_width, m_height);
            if (m_topBar) m_topBar->SetBounds(layout.topBar.left, layout.topBar.top,
                layout.topBar.right - layout.topBar.left,
                layout.topBar.bottom - layout.topBar.top);
            if (m_sidebar) m_sidebar->SetBounds(layout.sidebar.left, layout.sidebar.top,
                layout.sidebar.right - layout.sidebar.left,
                layout.sidebar.bottom - layout.sidebar.top);
            if (m_workspace) m_workspace->SetBounds(layout.workspace.left, layout.workspace.top,
                layout.workspace.right - layout.workspace.left,
                layout.workspace.bottom - layout.workspace.top);
            if (m_inspector) m_inspector->SetBounds(layout.inspector.left, layout.inspector.top,
                layout.inspector.right - layout.inspector.left,
                layout.inspector.bottom - layout.inspector.top);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            float mx = (float)pt.x, my = (float)pt.y;
            LayoutRects layout;
            ComputeLayout(layout, m_width, m_height);
            if (my < 48.0f && m_topBar) m_topBar->HandleClick(mx, my);
            else if (mx >= layout.sidebar.left && mx <= layout.sidebar.right && m_sidebar)
                m_sidebar->HandleClick(mx, my);
            else if (m_workspace) m_workspace->HandleClick(mx, my);
            else if (mx >= layout.inspector.left && mx <= layout.inspector.right && m_inspector)
                m_inspector->HandleClick(mx, my);
            return 0;
        }
        case WM_CHAR: {
            return 0;
        }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

}
