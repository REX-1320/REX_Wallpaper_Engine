#pragma once
#include <windows.h>
#include <d2d1_3.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <memory>
#include <atomic>
#include "shell/GlowStyle.h"
#include "shell/SpringAnimation.h"

namespace rex {
    namespace modules { class ConfigurationStore; class EventBus; }
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

class TopBar;
class Sidebar;
class WorkspaceHost;
class InspectorPanel;
class LiquidGlassRenderer;
class HomePage;
class SettingsPage;
class WallpaperPage;
class WidgetsPage;
class PersonalizationPage;
class DeveloperPage;
class PerformancePage;
class NotificationCenter;

struct LayoutRects;

class ShellWindow {
public:
    ShellWindow(
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
    ~ShellWindow();

    void Run();
    void RequestClose() { m_running = false; }

private:
    HWND m_hwnd = nullptr;
    std::atomic<bool> m_running{true};

    Microsoft::WRL::ComPtr<ID3D11Device> m_d3dDevice;
    Microsoft::WRL::ComPtr<IDCompositionDevice> m_dcompDevice;
    Microsoft::WRL::ComPtr<IDCompositionTarget> m_dcompTarget;
    Microsoft::WRL::ComPtr<IDCompositionVisual> m_rootVisual;
    Microsoft::WRL::ComPtr<ID2D1Device> m_d2dDevice;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> m_d2dContext;
    Microsoft::WRL::ComPtr<IDWriteFactory> m_dwriteFactory;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_glassTintBrush;
    Microsoft::WRL::ComPtr<ID2D1Effect> m_gaussianBlur;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> m_desktopCapture;

    int m_width = 0, m_height = 0;
    GlowStyle m_glowStyle;

    std::unique_ptr<TopBar> m_topBar;
    std::unique_ptr<Sidebar> m_sidebar;
    std::unique_ptr<WorkspaceHost> m_workspace;
    std::unique_ptr<InspectorPanel> m_inspector;
    std::unique_ptr<LiquidGlassRenderer> m_glass;

    std::unique_ptr<NotificationCenter> m_notifCenter;

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

    int m_activePage = 0;

    void InitializeWindow();
    void InitializeDComp();
    void CreateSurfaces();
    void ComputeLayout(LayoutRects& out, int clientW, int clientH);
    void HandleInput(float dt);
    void UpdatePhysics(float dt);
    void RenderFrame();

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(HWND, UINT, WPARAM, LPARAM);
};

}
