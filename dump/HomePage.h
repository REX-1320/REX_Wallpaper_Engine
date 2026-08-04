#pragma once
#include "PageRenderer.h"
#include "Card.h"
#include "ProgressBar.h"
#include "Gallery.h"
#include "Button.h"
#include "SectionHeader.h"
#include "PropertyGrid.h"
#include "GlowStyle.h"
#include "rex/wallpaper/IWallpaperEngine.hpp"
#include "modules/Widgets/interfaces/IWidgetService.hpp"
#include "modules/Personalization/interfaces/IPersonalizationService.hpp"
#include "modules/Settings/interfaces/ISettingsService.hpp"
#include "modules/Notification/interfaces/INotificationService.hpp"
#include <memory>
#include <chrono>

class HomePage : public PageRenderer {
public:
    HomePage(ID2D1RenderTarget* rt, IDWriteFactory* dwrite,
             const GlowStyle& style,
             std::shared_ptr<rex::wallpaper::IWallpaperEngine> wallpaperEngine,
             std::shared_ptr<rex::widgets::IWidgetService> widgetService,
             std::shared_ptr<rex::personalization::IPersonalizationService> personalizationService,
             std::shared_ptr<rex::settings::ISettingsService> settingsService,
             std::shared_ptr<rex::notifications::INotificationService> notificationService);
    void SetBounds(float x, float y, float w, float h);
    void Update(float dt) override;
    void Render(ID2D1RenderTarget* rt) override;
    void HandleClick(float mx, float my) override;

private:
    void RefreshData();

    std::shared_ptr<rex::wallpaper::IWallpaperEngine> m_wallpaperEngine;
    std::shared_ptr<rex::widgets::IWidgetService> m_widgetService;
    std::shared_ptr<rex::personalization::IPersonalizationService> m_personalizationService;
    std::shared_ptr<rex::settings::ISettingsService> m_settingsService;
    std::shared_ptr<rex::notifications::INotificationService> m_notificationService;

    Gallery m_recentGallery;
    ProgressBar m_downloadProgress;
    std::vector<Button> m_quickActions;

    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_titleFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_bodyFormat;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_textBrush;

    float m_x=0, m_y=0, m_w=0, m_h=0;

    std::wstring m_greeting = L"Welcome Back";
    std::wstring m_currentTime;
    std::wstring m_wallpaperName = L"None";
    std::wstring m_wallpaperType = L"Static";
    std::wstring m_wallpaperResolution = L"1920x1080";
    bool m_wallpaperPlaying = false;
    int m_runningWidgets = 0;
    std::wstring m_currentTheme = L"Midnight Glass";
    float m_cpuPercent = 0.0f, m_gpuPercent = 0.0f, m_ramPercent = 0.0f;
    float m_storagePercent = 0.0f;
    float m_downloadProgressValue = 0.0f;
    std::wstring m_downloadLabel = L"No active downloads";
    std::vector<std::wstring> m_activity;
    std::vector<GalleryItem> m_recentItems;

    std::chrono::steady_clock::time_point m_lastClockUpdate;
};
