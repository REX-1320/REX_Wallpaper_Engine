#include "TransparentShellWindow.hpp"
#include <windowsx.h>
#include <dwmapi.h>
#include <chrono>
#include <thread>
#include <algorithm>

static void Log(const char*) {}
static void LogHr(const char*, HRESULT) {}

#include "modules/UI/interfaces/IThemeService.hpp"
#include "modules/Settings/interfaces/ISettingsService.hpp"
#include "rex/wallpaper/IWallpaperEngine.hpp"
#include "modules/Notification/interfaces/INotificationService.hpp"
#include "modules/Command/interfaces/ICommandService.hpp"
#include "modules/Search/interfaces/ISearchService.hpp"
#include "modules/Widgets/interfaces/IWidgetService.hpp"
#include "modules/Personalization/interfaces/IPersonalizationService.hpp"
#include "graphics/effects/LiquidGlassBrush.hpp"


namespace rex::shell {

TransparentShellWindow::TransparentShellWindow(
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
      m_widgetService(widgetService),
      m_personalizationService(personalizationService),
      m_configStore(configStore), m_eventBus(eventBus),
      m_searchBar(nullptr, nullptr, m_glowStyle),
      m_toolbar(nullptr, nullptr, m_glowStyle),
      m_statusBar(nullptr, nullptr, m_glowStyle),
      m_breadcrumb(nullptr, nullptr, m_glowStyle),
      m_contextMenu(nullptr, nullptr, m_glowStyle),
      m_tooltip(nullptr, nullptr, m_glowStyle),
      m_mainSplit(nullptr, m_glowStyle),
      m_contentSplit(nullptr, m_glowStyle),
      m_notifCenter(nullptr, nullptr, m_glowStyle, nullptr)
{
    InitializeWindow();
    InitializeD2D();
    LoadThemeStyle();
    LoadSettings();

    if (m_rt && m_dwriteFactory) {
        m_searchBar = SearchBar(m_rt.Get(), m_dwriteFactory.Get(), m_glowStyle);
        m_toolbar = Toolbar(m_rt.Get(), m_dwriteFactory.Get(), m_glowStyle);
        m_statusBar = StatusBar(m_rt.Get(), m_dwriteFactory.Get(), m_glowStyle);
        m_breadcrumb = Breadcrumb(m_rt.Get(), m_dwriteFactory.Get(), m_glowStyle);
        m_contextMenu = ContextMenu(m_rt.Get(), m_dwriteFactory.Get(), m_glowStyle);
        m_tooltip = Tooltip(m_rt.Get(), m_dwriteFactory.Get(), m_glowStyle);
        m_mainSplit = SplitView(m_rt.Get(), m_glowStyle, SplitView::Orientation::Horizontal);
        m_contentSplit = SplitView(m_rt.Get(), m_glowStyle, SplitView::Orientation::Horizontal);

        CreateLayout();

        m_dotGrid.Build(m_width, m_height);

        m_commandDialog = std::make_unique<ModalDialog>(m_rt.Get(), m_dwriteFactory.Get(), m_glowStyle);
        m_commandDialog->SetTitle(L"Command Palette");
        m_commandDialog->SetSize(500, 400);

        m_homePage = std::make_unique<HomePage>(m_rt.Get(), m_dwriteFactory.Get(),
            m_glowStyle, m_wallpaperEngine, m_widgetService,
            m_personalizationService, m_settingsService, m_notificationService);

        m_wallpaperPage = std::make_unique<WallpaperPage>(m_rt.Get(), m_dwriteFactory.Get(),
            m_glowStyle, m_wallpaperEngine);
        m_wallpaperPage->ScanDirectory(L"C:\\Wallpapers");

        m_widgetsPage = std::make_unique<WidgetsPage>(m_rt.Get(), m_dwriteFactory.Get(),
            m_glowStyle, m_widgetService);

        m_personalizationPage = std::make_unique<PersonalizationPage>(m_rt.Get(), m_dwriteFactory.Get(),
            m_glowStyle, m_personalizationService);

        m_settingsPage = std::make_unique<SettingsPage>(m_rt.Get(), m_dwriteFactory.Get(),
            m_glowStyle, m_settingsService);

        m_notifCenter = NotificationCenter(m_rt.Get(), m_dwriteFactory.Get(), m_glowStyle, m_notificationService);

        auto getModules = []() -> std::vector<std::string> {
            return {"rex.core", "rex.shell", "rex.wallpaper", "rex.widgets",
                    "rex.settings", "rex.notification", "rex.command", "rex.search"};
        };
        auto getState = [](const std::string&) -> std::string {
            return "Running";
        };

        m_developerPage = std::make_unique<DeveloperPage>(m_rt.Get(), m_dwriteFactory.Get(),
            m_glowStyle, m_eventBus, m_commandService, m_searchService,
            m_notificationService, m_settingsService, getModules, getState);

        m_performancePage = std::make_unique<PerformancePage>(m_rt.Get(), m_dwriteFactory.Get(),
            m_glowStyle, m_wallpaperEngine, m_widgetService);

        m_syncEngine = std::make_shared<rex::sync::SyncEngine>();
        m_cloudSyncPage = std::make_unique<CloudSyncPage>(m_rt.Get(), m_dwriteFactory.Get(),
            m_glowStyle, m_syncEngine);
    }
}

TransparentShellWindow::~TransparentShellWindow() {
    if (m_hwnd) DestroyWindow(m_hwnd);
    if (m_hDib) DeleteObject(m_hDib);
    m_overlayBrush.Reset();
    m_d2dContext.Reset();
    m_d2dDevice.Reset();
    m_d3dContext.Reset();
    m_d3dDevice.Reset();
    m_rt.Reset();
    m_wicBitmap.Reset();
    m_wicFactory.Reset();
    m_d2dFactory1.Reset();
    m_d2dFactory.Reset();
    m_dwriteFactory.Reset();
    CoUninitialize();
}

void TransparentShellWindow::InitializeWindow() {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"TransparentShellWindow";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    RegisterClassW(&wc);

    m_width = GetSystemMetrics(SM_CXSCREEN);
    m_height = GetSystemMetrics(SM_CYSCREEN);

    m_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST,
        wc.lpszClassName, L"REX Shell",
        WS_POPUP,
        0, 0, m_width, m_height,
        nullptr, nullptr, wc.hInstance, this);
    if (m_hwnd) {
        ShowWindow(m_hwnd, SW_SHOW);
        UpdateWindow(m_hwnd);
        SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }
}

void TransparentShellWindow::InitializeD2D() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) { LogHr("CoInitializeEx", hr); return; }

    D2D1_FACTORY_OPTIONS options = {};
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
        __uuidof(ID2D1Factory1), &options, reinterpret_cast<void**>(m_d2dFactory1.GetAddressOf()));
    if (SUCCEEDED(hr) && m_d2dFactory1) {
        m_d2dFactory1.As(&m_d2dFactory);
    } else {
        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
            __uuidof(ID2D1Factory), reinterpret_cast<void**>(m_d2dFactory.GetAddressOf()));
        if (FAILED(hr) || !m_d2dFactory) { LogHr("D2D1CreateFactory", hr); return; }
    }

    // Initialize D3D11 Device for D2D1DeviceContext hardware acceleration & effect graph
    UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL featureLevel;
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        creationFlags, featureLevels, ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION, &m_d3dDevice, &featureLevel, &m_d3dContext);

    if (m_d3dDevice && m_d2dFactory1) {
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        if (SUCCEEDED(m_d3dDevice.As(&dxgiDevice))) {
            m_d2dFactory1->CreateDevice(dxgiDevice.Get(), &m_d2dDevice);
            if (m_d2dDevice) {
                m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &m_d2dContext);
            }
        }
    }

    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        __uuidof(IWICImagingFactory), reinterpret_cast<void**>(m_wicFactory.GetAddressOf()));
    if (FAILED(hr) || !m_wicFactory) { LogHr("CoCreateInstance WIC", hr); return; }

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf()));
    if (FAILED(hr) || !m_dwriteFactory) { LogHr("DWriteCreateFactory", hr); return; }

    hr = m_wicFactory->CreateBitmap(m_width, m_height, GUID_WICPixelFormat32bppPBGRA,
        WICBitmapCacheOnDemand, &m_wicBitmap);
    if (FAILED(hr) || !m_wicBitmap) { LogHr("CreateBitmap", hr); return; }

    m_rt = nullptr;
    hr = m_d2dFactory->CreateWicBitmapRenderTarget(m_wicBitmap.Get(),
        D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
        &m_rt);
    if (FAILED(hr) || !m_rt) { LogHr("CreateWicBitmapRenderTarget", hr); return; }

    m_rt->CreateSolidColorBrush(D2D1::ColorF(1,1,1,0.5f), &m_overlayBrush);
}

void TransparentShellWindow::LoadThemeStyle() {
    m_glowStyle.textColor = D2D1::ColorF(1,1,1,1);
    m_glowStyle.glowColor = D2D1::ColorF(0.6f,0.8f,1.0f,1);
    m_glowStyle.placeholderColor = D2D1::ColorF(0.5f,0.5f,0.5f,1);
    if (!m_themeService) return;
}

void TransparentShellWindow::LoadSettings() {
    if (!m_settingsService) return;
    rex::settings::Category shellCat{"shell", "Shell", "REX Shell settings", {}};
    m_settingsService->registerCategory(shellCat);
    m_settingsService->registerSetting({
        "shell.theme", "Theme", "UI colour theme",
        "shell", rex::settings::SettingType::String,
        std::string("dark"), std::string("dark")
    });
    m_settingsService->registerSetting({
        "shell.sidebar_collapsed", "Collapse Sidebar",
        "Collapse the sidebar by default",
        "shell", rex::settings::SettingType::Boolean,
        false, false
    });
}

void TransparentShellWindow::CreateLayout() {
    m_mainSplit.SetBounds(0, 0, (float)m_width, (float)m_height);
    m_mainSplit.SetSplitPosition(0.16f);

    m_contentSplit.SetSplitPosition(0.714f);

    m_toolbar.SetBounds(0, 0, (float)m_width);
    m_notifCenter.SetBounds((float)m_width - 340, TITLE_BAR_HEIGHT, 340, (float)m_height - TITLE_BAR_HEIGHT - 26);

    m_toolbar.SetActions({
        {L"Menu", L"\u2630", [](){}},
        {L"Search", L"?", [this](){ m_searchBar.HandleChar(L' '); }},
        {L"Notifications", L"\u2661", [this](){ m_notifCenter.Toggle(); }}
    });

    m_searchBar.SetPosition(60, 4, 400);
    m_searchBar.SetOnSearch([this](const std::wstring& query) {
        if (m_searchService) {
            m_searchService->search(rex::search::SearchQuery{std::string(query.begin(), query.end())});
        }
    });

    m_statusBar.SetBounds(0, (float)m_height - 26, (float)m_width);
    m_statusBar.SetLeftItems({{L"REX v1.0", nullptr}, {L"Ready", nullptr}});
    m_statusBar.SetRightItems({{L"CPU 0%", nullptr}});

    CreateSidebarGroups();
}

void TransparentShellWindow::CreateSidebarGroups() {
    m_sidebarGroups = {
        { L"HOME",        { L"Dashboard" }},
        { L"DESKTOP",     { L"Wallpapers", L"Widgets", L"Themes", L"Personalization" }},
        { L"DISCOVER",    { L"Marketplace", L"Downloads" }},
        { L"SYSTEM",      { L"Performance", L"Activity", L"Cloud Sync", L"Settings" }},
        { L"ADVANCED",    { L"Developer", L"About" }}
    };

    m_sidebarHeaders.clear();
    m_sidebarItems.clear();
    int flatIndex = 0;
    for (auto& group : m_sidebarGroups) {
        GlowTextButton headerBtn(group.header, m_rt.Get(), m_dwriteFactory.Get(), m_glowStyle);
        headerBtn.SetFontSize(11.0f);
        m_sidebarHeaders.push_back(std::move(headerBtn));

        for (size_t i = 0; i < group.items.size(); ++i) {
            GlowTextButton itemBtn(group.items[i], m_rt.Get(), m_dwriteFactory.Get(), m_glowStyle);
            itemBtn.SetFontSize(17.0f);
            itemBtn.SetOnClick([this, idx = flatIndex]() {
                m_activePage = idx;
                UpdateSidebarSelection();
            });
            m_sidebarItems.push_back(std::move(itemBtn));
            ++flatIndex;
        }
    }
    UpdateSidebarSelection();
}

void TransparentShellWindow::UpdateSidebarSelection() {
    for (int i = 0; i < (int)m_sidebarItems.size(); ++i) {
        m_sidebarItems[i].SetSelected(i == m_activePage);
    }
}

void TransparentShellWindow::Run() {
    MSG msg = {};
    auto lastTime = std::chrono::steady_clock::now();
    int frameCount = 0;
    while (m_running) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { m_running = false; break; }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        auto now = std::chrono::steady_clock::now();
        float delta = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;
        delta = std::min(delta, 0.05f);

        HandleInput();
        UpdatePhysics(delta);
        UpdateNotifications();
        RenderFrame();
        UpdateLayeredWindow();
        frameCount++;

        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
}

void TransparentShellWindow::HandleInput() {
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(m_hwnd, &pt);
    m_lastMouse = pt;
    float mx = (float)pt.x, my = (float)pt.y;

    bool leftDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (leftDown && !m_leftMouseDown) {
        for (auto& btn : m_sidebarItems) {
            if (btn.HitTest(mx, my)) {
                auto cb = btn.GetOnClick();
                if (cb) cb();
                break;
            }
        }
        if (m_notifCenter.HitTest(mx, my)) {
            m_notifCenter.HandleClick(mx, my);
        } else if (m_notifCenter.IsVisible()) {
            m_notifCenter.Hide();
        } else if (m_contextMenu.IsVisible()) {
            m_contextMenu.HandleClick(mx, my);
        } else if (m_commandDialog && m_commandDialog->IsVisible()) {
            m_commandDialog->HandleClick(mx, my);
        } else if (m_activePage == 0 && m_homePage) {
            m_homePage->HandleClick(mx, my);
        } else if (m_activePage == 1 && m_wallpaperPage) {
            m_wallpaperPage->HandleClick(mx, my);
        } else if (m_activePage == 2 && m_widgetsPage) {
            m_widgetsPage->HandleClick(mx, my);
        } else if (m_activePage == 4 && m_personalizationPage) {
            m_personalizationPage->HandleClick(mx, my);
        } else if (m_activePage == 7 && m_performancePage) {
            m_performancePage->HandleClick(mx, my);
        } else if (m_activePage == 9 && m_cloudSyncPage) {
            m_cloudSyncPage->HandleClick(mx, my);
        } else if (m_activePage == 10 && m_settingsPage) {
            m_settingsPage->HandleClick(mx, my);
        } else if (m_activePage == 11 && m_developerPage) {
            m_developerPage->HandleClick(mx, my);
        }
    }
    m_leftMouseDown = leftDown;

    if (GetAsyncKeyState(VK_RBUTTON) & 1) {
        m_contextMenu.SetItems({
            {L"Refresh", [](){}, true},
            {L"Settings", [](){}, true},
            {L"", nullptr, true, true},
            {L"Exit", [this](){ RequestClose(); }, true}
        });
        m_contextMenu.ShowAt(mx, my);
    }
}

void TransparentShellWindow::UpdatePhysics(float dt) {
    m_dotGrid.Update(dt, (float)m_lastMouse.x, (float)m_lastMouse.y, m_leftMouseDown);

    m_toolbar.Update(dt);
    m_searchBar.Update(dt);
    m_contextMenu.Update(dt);
    m_tooltip.Update(dt);
    if (m_commandDialog) m_commandDialog->Update(dt);
    if (m_homePage && m_activePage == 0) m_homePage->Update(dt);
    if (m_wallpaperPage && m_activePage == 1) m_wallpaperPage->Update(dt);
    if (m_widgetsPage && m_activePage == 2) m_widgetsPage->Update(dt);
    if (m_personalizationPage && m_activePage == 4) m_personalizationPage->Update(dt);
    if (m_performancePage && m_activePage == 7) m_performancePage->Update(dt);
    if (m_cloudSyncPage && m_activePage == 9) m_cloudSyncPage->Update(dt);
    if (m_settingsPage && m_activePage == 10) m_settingsPage->Update(dt);
    if (m_developerPage && m_activePage == 11) m_developerPage->Update(dt);
    if (m_syncEngine) m_syncEngine->Update(dt);
    m_notifCenter.Update(dt);
    for (auto& toast : m_toasts) toast.Update(dt);
    m_toasts.erase(
        std::remove_if(m_toasts.begin(), m_toasts.end(),
            [](const auto& t) { return t.IsExpired(); }),
        m_toasts.end());
}

void TransparentShellWindow::UpdateNotifications() {
    if (!m_notificationService) return;
    auto displayed = m_notificationService->getDisplayed();
    for (auto& n : displayed) {
        if (m_shownNotificationIds.find(n.id) != m_shownNotificationIds.end())
            continue;
        m_shownNotificationIds.insert(n.id);
        NotificationToast toast(
            std::wstring(n.title.begin(), n.title.end()),
            std::wstring(n.message.begin(), n.message.end()),
            m_rt.Get(), m_dwriteFactory.Get(), m_glowStyle);
        toast.SetOnDismiss([this, id = n.id]() {
            m_notificationService->dismiss(id);
            m_shownNotificationIds.erase(id);
        });
        m_toasts.push_back(std::move(toast));
        if (m_toasts.size() > 5)
            m_toasts.erase(m_toasts.begin());
    }
}

void TransparentShellWindow::RenderFrame() {
    if (!m_rt) return;
    m_rt->BeginDraw();
    m_rt->Clear(D2D1::ColorF(0.08f, 0.08f, 0.1f, 1.0f));

    m_dotGrid.Render(m_rt.Get());

    D2D1_RECT_F titleRect = {0, 0, (float)m_width, TITLE_BAR_HEIGHT};
    m_overlayBrush->SetColor(D2D1::ColorF(0, 0, 0, 0.55f));
    m_rt->FillRectangle(titleRect, m_overlayBrush.Get());
    m_overlayBrush->SetColor(D2D1::ColorF(1, 1, 1, 0.1f));
    m_rt->DrawLine(D2D1::Point2F(0, TITLE_BAR_HEIGHT), D2D1::Point2F((float)m_width, TITLE_BAR_HEIGHT),
        m_overlayBrush.Get(), 1.0f);
    if (m_dwriteFactory) {
        Microsoft::WRL::ComPtr<IDWriteTextFormat> tf;
        m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"en-us", &tf);
        if (tf) {
            m_overlayBrush->SetColor(m_glowStyle.textColor);
            m_rt->DrawText(L"REX Shell", 9, tf.Get(), D2D1::RectF(12.0f, 8.0f, 200.0f, TITLE_BAR_HEIGHT),
                m_overlayBrush.Get());
        }
    }

    m_toolbar.Render(m_rt.Get());
    m_searchBar.Render(m_rt.Get());

    m_mainSplit.SetBounds(0, TITLE_BAR_HEIGHT, (float)m_width,
        (float)m_height - TITLE_BAR_HEIGHT - 26);
    m_mainSplit.Render(m_rt.Get());

    float sx, sy, sw, sh, cx, cy, cw, ch;
    m_mainSplit.GetFirstPaneRect(sx, sy, sw, sh);
    m_mainSplit.GetSecondPaneRect(cx, cy, cw, ch);

    RenderSidebar(m_rt.Get());

    float contentY = cy;
    float contentH = ch;
    m_contentSplit.SetBounds(cx + 4, contentY, cw - 8, contentH);
    m_contentSplit.Render(m_rt.Get());

    float wx, wy, ww, wh, ix, iy, iw, ih;
    m_contentSplit.GetFirstPaneRect(wx, wy, ww, wh);
    m_contentSplit.GetSecondPaneRect(ix, iy, iw, ih);

    m_breadcrumb.SetBounds(wx + 8, wy);
    m_breadcrumb.Render(m_rt.Get());

    RenderMainWorkspace(m_rt.Get());
    RenderInspector(m_rt.Get());

    m_statusBar.Render(m_rt.Get());

    if (m_commandDialog && m_commandDialog->IsVisible())
        m_commandDialog->Render(m_rt.Get());
    m_contextMenu.Render(m_rt.Get());
    m_tooltip.Render(m_rt.Get());
    m_notifCenter.Render(m_rt.Get());

    float notifX = (float)m_width - 300, notifY = (float)m_height - 120;
    for (auto& toast : m_toasts) {
        toast.SetPosition(notifX, notifY);
        toast.Render(m_rt.Get());
        notifY -= 80;
    }

    m_rt->EndDraw();
}

void TransparentShellWindow::RenderSidebar(ID2D1RenderTarget* rt) {
    float sx, sy, sw, sh;
    m_mainSplit.GetFirstPaneRect(sx, sy, sw, sh);

    D2D1_ROUNDED_RECT sidebarGlass = D2D1::RoundedRect(
        D2D1::RectF(sx + 4.0f, sy + 4.0f, sx + sw - 4.0f, sy + sh - 4.0f),
        16.0f, 16.0f
    );
    rex::graphics::LiquidGlassBrush::RenderBaseSurface(rt, sidebarGlass, m_glassTokens);

    float x = sx + 16, y = sy + 16;

    int flatIndex = 0;
    size_t groupIdx = 0;
    for (auto& group : m_sidebarGroups) {
        if (groupIdx < m_sidebarHeaders.size()) {
            m_sidebarHeaders[groupIdx].SetPosition(x + 4, y);
            m_sidebarHeaders[groupIdx].Render(rt);
            y += 24;
        }

        for (size_t i = 0; i < group.items.size(); ++i) {
            if (flatIndex < (int)m_sidebarItems.size()) {
                m_sidebarItems[flatIndex].SetPosition(x + 14, y);
                m_sidebarItems[flatIndex].SetSelected(flatIndex == m_activePage);
                m_sidebarItems[flatIndex].Render(rt);
                y += 30;
            }
            ++flatIndex;
        }
        ++groupIdx;
        y += 6;
    }
}

void TransparentShellWindow::RenderMainWorkspace(ID2D1RenderTarget* rt) {
    float wx, wy, ww, wh;
    m_contentSplit.GetFirstPaneRect(wx, wy, ww, wh);
    float workspaceX = wx + 8, workspaceY = wy + 32;
    float workspaceW = ww - 16, workspaceH = wh - 32;

    if (m_activePage == 0 && m_homePage) {
        m_homePage->SetBounds(workspaceX, workspaceY, workspaceW, workspaceH);
        m_homePage->Render(rt);
    } else if (m_activePage == 1 && m_wallpaperPage) {
        m_wallpaperPage->SetBounds(workspaceX, workspaceY, workspaceW, workspaceH);
        m_wallpaperPage->Render(rt);
    } else if (m_activePage == 2 && m_widgetsPage) {
        m_widgetsPage->SetBounds(workspaceX, workspaceY, workspaceW, workspaceH);
        m_widgetsPage->Render(rt);
    } else if (m_activePage == 4 && m_personalizationPage) {
        m_personalizationPage->SetBounds(workspaceX, workspaceY, workspaceW, workspaceH);
        m_personalizationPage->Render(rt);
    } else if (m_activePage == 7 && m_performancePage) {
        m_performancePage->SetBounds(workspaceX, workspaceY, workspaceW, workspaceH);
        m_performancePage->Render(rt);
    } else if (m_activePage == 9 && m_cloudSyncPage) {
        m_cloudSyncPage->SetBounds(workspaceX, workspaceY, workspaceW, workspaceH);
        m_cloudSyncPage->Render(rt);
    } else if (m_activePage == 10 && m_settingsPage) {
        m_settingsPage->SetBounds(workspaceX, workspaceY, workspaceW, workspaceH);
        m_settingsPage->Render(rt);
    } else if (m_activePage == 11 && m_developerPage) {
        m_developerPage->SetBounds(workspaceX, workspaceY, workspaceW, workspaceH);
        m_developerPage->Render(rt);
    }
}

void TransparentShellWindow::RenderInspector(ID2D1RenderTarget* rt) {
    float ix, iy, iw, ih;
    m_contentSplit.GetSecondPaneRect(ix, iy, iw, ih);
    PropertyGrid propGrid(rt, m_dwriteFactory.Get(), m_glowStyle);
    propGrid.SetBounds(ix + 4, iy + 4, iw - 8);

    if (m_activePage == 0) {
        std::vector<PropertyRow> props;
        props.push_back({L"System", L"Online"});
        props.push_back({L"GPU", L"OK"});
        props.push_back({L"Modules", L"Running 8"});
        props.push_back({L"Updates", L"Available"});
        props.push_back({L"Version", L"1.0"});
        propGrid.SetProperties(props);
    } else if (m_activePage == 10) {
        std::vector<PropertyRow> props;
        props.push_back({L"Developer Mode", L"Active"});
        props.push_back({L"Event Subscribers", L"1"});
        props.push_back({L"Registered Commands", L"-"});
        props.push_back({L"Tabs", L"7"});
        propGrid.SetProperties(props);
    } else {
        propGrid.SetProperties({{L"Info", L"Select an item"}});
    }
    propGrid.Render(rt);
}

void TransparentShellWindow::UpdateLayeredWindow() {
    if (!m_wicBitmap || !m_hwnd) return;
    UINT w, h;
    m_wicBitmap->GetSize(&w, &h);

    if (!m_hDib || m_dibWidth != w || m_dibHeight != h) {
        if (m_hDib) { DeleteObject(m_hDib); m_hDib = nullptr; }
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = (LONG)w;
        bmi.bmiHeader.biHeight = -(LONG)h;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        HDC hdcMem = CreateCompatibleDC(nullptr);
        m_hDib = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &m_dibBits, nullptr, 0);
        DeleteDC(hdcMem);
        m_dibWidth = w; m_dibHeight = h;
    }

    if (m_dibBits) {
        WICRect rc = {0, 0, (INT)w, (INT)h};
        m_wicBitmap->CopyPixels(&rc, w * 4, w * h * 4, (BYTE*)m_dibBits);
    }

    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    SelectObject(hdcMem, m_hDib);

    RECT wr; GetWindowRect(m_hwnd, &wr);
    POINT ptDst = {wr.left, wr.top};
    SIZE sizeWnd = {wr.right - wr.left, wr.bottom - wr.top};
    POINT ptSrc = {0, 0};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    ::UpdateLayeredWindow(m_hwnd, hdcScreen, &ptDst, &sizeWnd, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);

    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);
}

LRESULT CALLBACK TransparentShellWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    TransparentShellWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        CREATESTRUCT* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        self = static_cast<TransparentShellWindow*>(cs->lpCreateParams);
        self->m_hwnd = hwnd;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    self = reinterpret_cast<TransparentShellWindow*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (self) return self->HandleMessage(hwnd, msg, wParam, lParam);
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

LRESULT TransparentShellWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_NCHITTEST: {
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &pt);
            if (pt.y >= 0 && pt.y < (int)TITLE_BAR_HEIGHT)
                return HTCAPTION;
            return HTCLIENT;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_CHAR:
            if (m_activePage == 9 && m_settingsPage) {
                m_settingsPage->HandleChar((wchar_t)wParam);
                return 0;
            }
            break;
        case WM_KEYDOWN: {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (ctrl && wParam == 'N') {
                m_notifCenter.Toggle();
                return 0;
            }
            if (ctrl && wParam == 'P') {
                if (m_commandDialog) m_commandDialog->Show();
                return 0;
            }
            if (wParam == VK_ESCAPE) {
                if (m_commandDialog && m_commandDialog->IsVisible())
                    m_commandDialog->Close();
                return 0;
            }
            if (m_activePage == 9 && m_settingsPage) {
                m_settingsPage->HandleKeyDown((int)wParam);
                return 0;
            }
            break;
        }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

}
