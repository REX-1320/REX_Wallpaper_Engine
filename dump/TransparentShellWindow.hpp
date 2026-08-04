#pragma once
#include <windows.h>
#include <d2d1_3.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dwrite_3.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <vector>
#include <memory>
#include <string>
#include <atomic>
#include <functional>
#include <unordered_set>
#include "SpringAnimation.h"
#include "GlowStyle.h"
#include "LiquidGlassTokens.hpp"
#include "GlowTextButton.h"
#include "GlowTextInput.h"
#include "NotificationToast.h"
#include "DotGridBackground.h"
#include "modules/ConfigurationStore.hpp"
#include "modules/EventBus.hpp"

#include "Button.h"
#include "Card.h"
#include "SectionHeader.h"
#include "Toggle.h"
#include "Slider.h"
#include "SearchBar.h"
#include "Gallery.h"
#include "Table.h"
#include "TreeView.h"
#include "PropertyGrid.h"
#include "ProgressBar.h"
#include "Skeleton.h"
#include "Badge.h"
#include "ModalDialog.h"
#include "AlertDialog.h"
#include "ColorPicker.h"
#include "Dropdown.h"
#include "Tooltip.h"
#include "ContextMenu.h"
#include "Tabs.h"
#include "Breadcrumb.h"
#include "Toolbar.h"
#include "StatusBar.h"
#include "SplitView.h"
#include "HomePage.h"
#include "SettingsPage.h"
#include "WallpaperPage.h"
#include "WidgetsPage.h"
#include "PersonalizationPage.h"
#include "DeveloperPage.h"
#include "NotificationCenter.h"
#include "PerformancePage.h"
#include "CloudSyncPage.hpp"

namespace rex {
    namespace ui { class IThemeService; }
    namespace search { class ISearchService; }
    namespace command { class ICommandService; }
    namespace notifications { class INotificationService; }
    namespace wallpaper { class IWallpaperEngine; }
    namespace settings { class ISettingsService; }
    namespace widgets { class IWidgetService; }
    namespace personalization { class IPersonalizationService; }
}

namespace rex::shell {

class TransparentShellWindow {
public:
    TransparentShellWindow(
        std::shared_ptr<rex::ui::IThemeService> themeService,
        std::shared_ptr<rex::search::ISearchService> searchService,
        std::shared_ptr<rex::command::ICommandService> commandService,
        std::shared_ptr<rex::notifications::INotificationService> notificationService,
        std::shared_ptr<rex::wallpaper::IWallpaperEngine> wallpaperEngine,
        std::shared_ptr<rex::settings::ISettingsService> settingsService,
        std::shared_ptr<rex::widgets::IWidgetService> widgetService,
        std::shared_ptr<rex::personalization::IPersonalizationService> personalizationService,
        std::shared_ptr<rex::modules::ConfigurationStore> configStore,
        std::shared_ptr<rex::modules::EventBus> eventBus);
    ~TransparentShellWindow();

    void Run();
    void RequestClose() { m_running = false; }

private:
    HWND m_hwnd = nullptr;
    int m_width = 1280, m_height = 720;
    std::atomic<bool> m_running{true};

    Microsoft::WRL::ComPtr<ID2D1Factory1> m_d2dFactory1;
    Microsoft::WRL::ComPtr<ID3D11Device> m_d3dDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_d3dContext;
    Microsoft::WRL::ComPtr<ID2D1Device> m_d2dDevice;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> m_d2dContext;
    Microsoft::WRL::ComPtr<ID2D1Factory> m_d2dFactory;
    Microsoft::WRL::ComPtr<IWICImagingFactory> m_wicFactory;
    Microsoft::WRL::ComPtr<IWICBitmap> m_wicBitmap;
    Microsoft::WRL::ComPtr<ID2D1RenderTarget> m_rt;
    Microsoft::WRL::ComPtr<IDWriteFactory> m_dwriteFactory;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_overlayBrush;
    LiquidGlassTokens m_glassTokens = LiquidGlassTokens::DefaultDark();

    HBITMAP m_hDib = nullptr;
    void* m_dibBits = nullptr;
    UINT m_dibWidth = 0, m_dibHeight = 0;

    std::shared_ptr<rex::ui::IThemeService> m_themeService;
    std::shared_ptr<rex::search::ISearchService> m_searchService;
    std::shared_ptr<rex::command::ICommandService> m_commandService;
    std::shared_ptr<rex::notifications::INotificationService> m_notificationService;
    std::shared_ptr<rex::wallpaper::IWallpaperEngine> m_wallpaperEngine;
    std::shared_ptr<rex::settings::ISettingsService> m_settingsService;
    std::shared_ptr<rex::widgets::IWidgetService> m_widgetService;
    std::shared_ptr<rex::personalization::IPersonalizationService> m_personalizationService;
    std::shared_ptr<rex::modules::ConfigurationStore> m_configStore;
    std::shared_ptr<rex::modules::EventBus> m_eventBus;

    static constexpr float TITLE_BAR_HEIGHT = 36.0f;
    GlowStyle m_glowStyle;

    DotGridBackground m_dotGrid;

    SplitView m_mainSplit;
    SplitView m_contentSplit;
    Toolbar m_toolbar;
    SearchBar m_searchBar;
    StatusBar m_statusBar;
    Breadcrumb m_breadcrumb;
    ContextMenu m_contextMenu;
    Tooltip m_tooltip;

    struct SidebarGroup {
        std::wstring header;
        std::vector<std::wstring> items;
    };
    int m_activePage = 0;
    std::vector<SidebarGroup> m_sidebarGroups;
    std::vector<GlowTextButton> m_sidebarHeaders;
    std::vector<GlowTextButton> m_sidebarItems;

    std::unique_ptr<ModalDialog> m_commandDialog;

    std::unique_ptr<HomePage> m_homePage;
    std::unique_ptr<SettingsPage> m_settingsPage;
    std::unique_ptr<WallpaperPage> m_wallpaperPage;
    std::unique_ptr<WidgetsPage> m_widgetsPage;
    std::unique_ptr<PersonalizationPage> m_personalizationPage;
    std::unique_ptr<DeveloperPage> m_developerPage;
    std::unique_ptr<PerformancePage> m_performancePage;
    std::unique_ptr<CloudSyncPage> m_cloudSyncPage;

    std::shared_ptr<rex::sync::SyncEngine> m_syncEngine;

    NotificationCenter m_notifCenter;
    std::vector<NotificationToast> m_toasts;
    std::unordered_set<std::string> m_shownNotificationIds;

    bool m_leftMouseDown = false;
    POINT m_lastMouse = {};

    void InitializeWindow();
    void InitializeD2D();
    void LoadThemeStyle();
    void LoadSettings();
    void CreateLayout();
    void CreateSidebarGroups();
    void UpdateSidebarSelection();
    void UpdatePhysics(float dt);
    void HandleInput();
    void UpdateNotifications();
    void RenderFrame();
    void RenderSidebar(ID2D1RenderTarget* rt);
    void RenderMainWorkspace(ID2D1RenderTarget* rt);
    void RenderInspector(ID2D1RenderTarget* rt);
    void UpdateLayeredWindow();

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(HWND, UINT, WPARAM, LPARAM);
};

}
