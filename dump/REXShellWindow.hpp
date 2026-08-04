#pragma once
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <memory>
#include "modules/UI/interfaces/IThemeService.hpp"
#include "modules/Search/interfaces/ISearchService.hpp"
#include "modules/Search/models/SearchResult.hpp"
#include "modules/Command/interfaces/ICommandService.hpp"
#include "modules/Command/models/CommandInstance.hpp"
#include "modules/Notification/interfaces/INotificationService.hpp"
#include "modules/Notification/models/Notification.hpp"
#include <d2d1helper.h>

namespace rex::shell {

class REXShellWindow {
public:
    REXShellWindow(
        std::shared_ptr<rex::ui::IThemeService> themeService,
        std::shared_ptr<rex::search::ISearchService> searchService,
        std::shared_ptr<rex::command::ICommandService> commandService,
        std::shared_ptr<rex::notifications::INotificationService> notificationService);
    ~REXShellWindow();

    void Show();

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void InitializeWindow();
    void CreateRenderTarget();
    void Render();
    void OnResize(int width, int height);

    void OnSearchTextChanged(const std::wstring& text);
    void RenderSearchBar();
    void RenderSearchResults();

    void ToggleCommandPalette();
    void ExecuteCommand(const std::string& commandId);
    void FilterCommands(const std::wstring& query);
    void RenderCommandPalette();

    void RenderNotifications();

    HWND m_hwnd = nullptr;
    int m_width = 1280, m_height = 720;

    Microsoft::WRL::ComPtr<ID2D1Factory> m_d2dFactory;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> m_renderTarget;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_backgroundBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_sidebarBrush;
    Microsoft::WRL::ComPtr<IDWriteFactory> m_dwriteFactory;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_textFormat;

    std::shared_ptr<rex::ui::IThemeService> m_themeService;
    std::shared_ptr<rex::search::ISearchService> m_searchService;
    std::shared_ptr<rex::command::ICommandService> m_commandService;
    std::shared_ptr<rex::notifications::INotificationService> m_notificationService;

    std::vector<std::string> m_modules = {
        "Home", "Wallpaper", "Personalization", "Widgets",
        "Marketplace", "Settings", "Developer"
    };
    int m_selectedIndex = 0;

    std::wstring m_searchQuery;
    std::vector<rex::search::SearchResult> m_searchResults;
    bool m_searchActive = false;
    bool m_searchFocused = false;

    std::wstring m_commandQuery;
    std::vector<rex::command::CommandInstance> m_filteredCommands;
    bool m_commandPaletteOpen = false;
};

} // namespace
