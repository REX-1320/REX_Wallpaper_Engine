#include "REXShellWindow.hpp"
#include <cstdio>

enum ACCENTSTATE {
    ACCENT_DISABLED = 0,
    ACCENT_ENABLE_GRADIENT = 1,
    ACCENT_ENABLE_TRANSPARENTGRADIENT = 2,
    ACCENT_ENABLE_BLURBEHIND = 3,
    ACCENT_ENABLE_ACRYLICBLURBEHIND = 4
};

struct ACCENTPOLICY {
    int AccentState;
    int AccentFlags;
    int GradientColor;
    int AnimationId;
};

struct WINCOMPATTRDATA {
    int attribute;
    void* data;
    int sizeOfData;
};

namespace rex::shell {

REXShellWindow::REXShellWindow(
    std::shared_ptr<rex::ui::IThemeService> themeService,
    std::shared_ptr<rex::search::ISearchService> searchService,
    std::shared_ptr<rex::command::ICommandService> commandService,
    std::shared_ptr<rex::notifications::INotificationService> notificationService)
    : m_themeService(themeService), m_searchService(searchService),
      m_commandService(commandService), m_notificationService(notificationService) {
    InitializeWindow();
    CreateRenderTarget();
}

REXShellWindow::~REXShellWindow() {
    if (m_hwnd) DestroyWindow(m_hwnd);
}

void REXShellWindow::Show() {
    ShowWindow(m_hwnd, SW_SHOWNORMAL);
    UpdateWindow(m_hwnd);
}

void REXShellWindow::InitializeWindow() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"REXShellWindow";
    RegisterClassExW(&wc);

    m_hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"REX Shell",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, m_width, m_height,
        nullptr, nullptr, wc.hInstance, this);

    if (!m_hwnd) return;

    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(m_hwnd, 19, &darkMode, sizeof(darkMode));

    DWM_BLURBEHIND bb = {};
    bb.dwFlags = DWM_BB_ENABLE;
    bb.fEnable = TRUE;
    bb.hRgnBlur = NULL;
    DwmEnableBlurBehindWindow(m_hwnd, &bb);

    ACCENTPOLICY policy = {ACCENT_ENABLE_ACRYLICBLURBEHIND, 0, 0, 0};
    WINCOMPATTRDATA data;
    data.attribute = 19;
    data.data = &policy;
    data.sizeOfData = sizeof(policy);

    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        using SetWindowCompositionAttribute_t = BOOL(WINAPI*)(HWND, WINCOMPATTRDATA*);
        auto fn = (SetWindowCompositionAttribute_t)GetProcAddress(user32, "SetWindowCompositionAttribute");
        if (fn) fn(m_hwnd, &data);
    }

    RECT rc;
    GetClientRect(m_hwnd, &rc);
    m_width = rc.right - rc.left;
    m_height = rc.bottom - rc.top;
}

void REXShellWindow::CreateRenderTarget() {
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
        __uuidof(ID2D1Factory),
        reinterpret_cast<void**>(m_d2dFactory.GetAddressOf()));
    if (!m_d2dFactory) return;

    RECT rc;
    GetClientRect(m_hwnd, &rc);
    D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);

    m_d2dFactory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
        D2D1::HwndRenderTargetProperties(m_hwnd, size),
        &m_renderTarget);

    std::string surfaceToken = m_themeService->getToken("Color.SurfaceBackground");
    std::string sidebarToken = m_themeService->getToken("Color.SidebarBackground");

    if (surfaceToken.empty()) surfaceToken = "rgba(54,69,79,0.5)";
    if (sidebarToken.empty()) sidebarToken = "#1E1E1E";

    auto parseColor = [](const std::string& token, float& r, float& g, float& b, float& a) {
        if (token.size() > 5 && token.substr(0, 5) == "rgba(") {
            float vals[4];
            sscanf_s(token.c_str(), "rgba(%f,%f,%f,%f)", &vals[0], &vals[1], &vals[2], &vals[3]);
            r = vals[0] / 255.0f; g = vals[1] / 255.0f;
            b = vals[2] / 255.0f; a = vals[3];
        } else if (token.size() > 0 && token[0] == '#') {
            unsigned int hex;
            sscanf_s(token.c_str() + 1, "%x", &hex);
            r = ((hex >> 16) & 0xFF) / 255.0f;
            g = ((hex >> 8) & 0xFF) / 255.0f;
            b = (hex & 0xFF) / 255.0f;
            a = 1.0f;
        }
    };

    float r, g, b, a;
    parseColor(surfaceToken, r, g, b, a);
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(54/255.0f, 69/255.0f, 79/255.0f, 0.5f), &m_backgroundBrush);

    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(30/255.0f, 30/255.0f, 30/255.0f, 0.6f), &m_sidebarBrush);

    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf()));

    m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr,
                                       DWRITE_FONT_WEIGHT_NORMAL,
                                       DWRITE_FONT_STYLE_NORMAL,
                                       DWRITE_FONT_STRETCH_NORMAL, 14.0f,
                                       L"en-us", &m_textFormat);
    m_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
}

void REXShellWindow::Render() {
    if (!m_renderTarget) return;
    m_renderTarget->BeginDraw();
    m_renderTarget->Clear(D2D1::ColorF(0, 0, 0, 0));

    D2D1_RECT_F sidebarRect = D2D1::RectF(0, 0, 250, (float)m_height);
    m_renderTarget->FillRectangle(&sidebarRect, m_sidebarBrush.Get());

    D2D1_RECT_F itemRect = D2D1::RectF(10, 50, 240, 90);
    for (size_t i = 0; i < m_modules.size(); ++i) {
        if (i == (size_t)m_selectedIndex) {
            m_renderTarget->FillRectangle(&itemRect, m_backgroundBrush.Get());
        }
        std::wstring text(m_modules[i].begin(), m_modules[i].end());
        m_renderTarget->DrawTextW(text.c_str(), (UINT32)text.size(),
                                   m_textFormat.Get(), &itemRect,
                                   m_backgroundBrush.Get());
        itemRect.top += 50;
        itemRect.bottom += 50;
    }

    D2D1_RECT_F topBar = D2D1::RectF(250, 0, (float)m_width, 40);
    m_renderTarget->FillRectangle(&topBar, m_sidebarBrush.Get());

    RenderSearchBar();

    D2D1_RECT_F workspaceRect = D2D1::RectF(250, 40, (float)m_width, (float)m_height);
    m_renderTarget->FillRectangle(&workspaceRect, m_backgroundBrush.Get());

    if (m_searchActive && !m_searchResults.empty()) {
        RenderSearchResults();
    }

    if (m_commandPaletteOpen) {
        RenderCommandPalette();
    }

    RenderNotifications();

    m_renderTarget->EndDraw();
}

void REXShellWindow::RenderSearchBar() {
    D2D1_RECT_F searchRect = D2D1::RectF(260, 5, 600, 35);
    m_renderTarget->DrawRectangle(&searchRect, m_backgroundBrush.Get());
    m_renderTarget->FillRectangle(&searchRect, m_backgroundBrush.Get());

    if (m_searchQuery.empty() && !m_searchFocused) {
        std::wstring placeholder = L"Search...  (Ctrl+F)";
        m_renderTarget->DrawTextW(placeholder.c_str(), (UINT32)placeholder.size(),
                                   m_textFormat.Get(), &searchRect, m_sidebarBrush.Get());
    } else {
        m_renderTarget->DrawTextW(m_searchQuery.c_str(), (UINT32)m_searchQuery.size(),
                                   m_textFormat.Get(), &searchRect, m_backgroundBrush.Get());
    }
    if (m_searchActive && !m_searchResults.empty()) {
        RenderSearchResults();
    }
}

void REXShellWindow::RenderSearchResults() {
    float yOff = 35.0f;
    D2D1_RECT_F resultRect = D2D1::RectF(260, yOff, 600,
                                          yOff + m_searchResults.size() * 30.0f);
    m_renderTarget->FillRectangle(&resultRect, m_sidebarBrush.Get());

    for (size_t i = 0; i < m_searchResults.size(); ++i) {
        D2D1_RECT_F itemRect = D2D1::RectF(260, yOff + i * 30.0f,
                                            600, yOff + i * 30.0f + 28.0f);
        std::wstring title(m_searchResults[i].title.begin(),
                           m_searchResults[i].title.end());
        m_renderTarget->DrawTextW(title.c_str(), (UINT32)title.size(),
                                   m_textFormat.Get(), &itemRect,
                                   m_backgroundBrush.Get());
    }
}

static std::string WideToUTF8(const std::wstring& wstr) {
    if (wstr.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(),
                                   nullptr, 0, nullptr, nullptr);
    std::string result((size_t)len, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(),
                         &result[0], len, nullptr, nullptr);
    return result;
}

void REXShellWindow::OnSearchTextChanged(const std::wstring& text) {
    m_searchQuery = text;
    if (text.empty()) {
        m_searchResults.clear();
        m_searchActive = false;
        return;
    }
    rex::search::SearchQuery sq;
    sq.rawText = WideToUTF8(text);
    sq.maxResults = 5;
    m_searchResults = m_searchService->search(sq);
    m_searchActive = true;
}

void REXShellWindow::ToggleCommandPalette() {
    m_commandPaletteOpen = !m_commandPaletteOpen;
    if (m_commandPaletteOpen) {
        m_commandQuery.clear();
        FilterCommands(L"");
    }
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void REXShellWindow::ExecuteCommand(const std::string& commandId) {
    m_commandService->execute(commandId);
    m_commandPaletteOpen = false;
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void REXShellWindow::FilterCommands(const std::wstring& query) {
    m_commandQuery = query;
    auto allCommands = m_commandService->getAllCommands();
    m_filteredCommands.clear();
    std::string q = WideToUTF8(query);
    for (auto& cmd : allCommands) {
        if (!cmd.enabled || !cmd.visible) continue;
        if (q.empty() || cmd.definition->name.find(q) != std::string::npos) {
            m_filteredCommands.push_back(cmd);
        }
    }
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void REXShellWindow::RenderCommandPalette() {
    D2D1_RECT_F overlay = D2D1::RectF(0, 0, (float)m_width, (float)m_height);
    ID2D1SolidColorBrush* overlayBrush = nullptr;
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.5f), &overlayBrush);
    if (overlayBrush) {
        m_renderTarget->FillRectangle(&overlay, overlayBrush);
        overlayBrush->Release();
    }

    D2D1_RECT_F box = D2D1::RectF(200, 100, (float)m_width - 200, 400);
    m_renderTarget->FillRectangle(&box, m_sidebarBrush.Get());

    D2D1_RECT_F inputRect = D2D1::RectF(210, 110, (float)m_width - 210, 140);
    m_renderTarget->FillRectangle(&inputRect, m_backgroundBrush.Get());

    if (m_commandQuery.empty()) {
        std::wstring placeholder = L"Type a command...";
        m_renderTarget->DrawTextW(placeholder.c_str(), (UINT32)placeholder.size(),
                                   m_textFormat.Get(), &inputRect, m_sidebarBrush.Get());
    } else {
        m_renderTarget->DrawTextW(m_commandQuery.c_str(), (UINT32)m_commandQuery.size(),
                                   m_textFormat.Get(), &inputRect, m_backgroundBrush.Get());
    }

    for (size_t i = 0; i < m_filteredCommands.size(); ++i) {
        D2D1_RECT_F cmdRect = D2D1::RectF(210, 150 + i * 30,
                                           (float)m_width - 210, 180 + i * 30);
        std::wstring name(m_filteredCommands[i].definition->name.begin(),
                          m_filteredCommands[i].definition->name.end());
        m_renderTarget->DrawTextW(name.c_str(), (UINT32)name.size(),
                                   m_textFormat.Get(), &cmdRect, m_backgroundBrush.Get());
    }
}

void REXShellWindow::RenderNotifications() {
    auto notifications = m_notificationService->getDisplayed();
    if (notifications.empty()) return;

    float startY = (float)m_height - 100;
    for (size_t i = 0; i < notifications.size(); ++i) {
        D2D1_RECT_F toast = D2D1::RectF((float)m_width - 300, startY - i * 80,
                                          (float)m_width - 20, startY - i * 80 + 70);
        m_renderTarget->FillRectangle(&toast, m_sidebarBrush.Get());
        std::wstring title(notifications[i].title.begin(),
                           notifications[i].title.end());
        m_renderTarget->DrawTextW(title.c_str(), (UINT32)title.size(),
                                   m_textFormat.Get(), &toast,
                                   m_backgroundBrush.Get());
    }
}

void REXShellWindow::OnResize(int width, int height) {
    m_width = width;
    m_height = height;
    if (m_renderTarget) {
        m_renderTarget->Resize(D2D1::SizeU((UINT32)width, (UINT32)height));
    }
}

LRESULT REXShellWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    REXShellWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        CREATESTRUCT* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        self = static_cast<REXShellWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<REXShellWindow*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) {
        return self->HandleMessage(hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT REXShellWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE:
        OnResize(LOWORD(lParam), HIWORD(lParam));
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        Render();
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        int x = LOWORD(lParam);
        int y = HIWORD(lParam);
        if (x < 250) {
            int index = (y - 50) / 50;
            if (index >= 0 && index < (int)m_modules.size()) {
                m_selectedIndex = index;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    }

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            if (m_commandPaletteOpen) {
                ToggleCommandPalette();
            } else if (m_searchActive) {
                m_searchActive = false;
                m_searchQuery.clear();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        if (wParam == VK_RETURN) {
            if (m_commandPaletteOpen && !m_filteredCommands.empty()) {
                ExecuteCommand(m_filteredCommands[0].id);
                return 0;
            }
            if (m_searchFocused && !m_searchResults.empty()) {
                m_searchFocused = false;
                m_searchActive = false;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
        }
        if (wParam == VK_BACK) {
            if (m_commandPaletteOpen && !m_commandQuery.empty()) {
                std::wstring q = m_commandQuery.substr(0, m_commandQuery.size() - 1);
                FilterCommands(q);
                return 0;
            }
            if (m_searchFocused && !m_searchQuery.empty()) {
                std::wstring q = m_searchQuery.substr(0, m_searchQuery.size() - 1);
                OnSearchTextChanged(q);
                return 0;
            }
        }
        break;

    case WM_CHAR: {
        wchar_t ch = (wchar_t)wParam;
        if (ch < 32) break;
        if (m_commandPaletteOpen) {
            FilterCommands(m_commandQuery + ch);
            return 0;
        }
        if (m_searchFocused) {
            OnSearchTextChanged(m_searchQuery + ch);
            return 0;
        }
        break;
    }

    case WM_SYSKEYDOWN:
        if (wParam == 'P' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            ToggleCommandPalette();
            return 0;
        }
        if (wParam == 'F' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            m_searchFocused = true;
            SetFocus(m_hwnd);
            return 0;
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace
