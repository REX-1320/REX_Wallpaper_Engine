#pragma once
#include "PageRenderer.h"
#include "Card.h"
#include "ProgressBar.h"
#include "SectionHeader.h"
#include "Table.h"
#include "Toggle.h"
#include "rex/wallpaper/IWallpaperEngine.hpp"
#include "modules/Widgets/interfaces/IWidgetService.hpp"
#include <memory>
#include <vector>
#include <string>
#include <chrono>

class PerformancePage : public PageRenderer {
public:
    PerformancePage(ID2D1RenderTarget* rt, IDWriteFactory* dwrite,
                    const GlowStyle& style,
                    std::shared_ptr<rex::wallpaper::IWallpaperEngine> wallpaperEngine,
                    std::shared_ptr<rex::widgets::IWidgetService> widgetService);
    void SetBounds(float x, float y, float w, float h);
    void Update(float dt) override;
    void Render(ID2D1RenderTarget* rt) override;
    void HandleClick(float mx, float my) override;

private:
    std::shared_ptr<rex::wallpaper::IWallpaperEngine> m_wallpaperEngine;
    std::shared_ptr<rex::widgets::IWidgetService> m_widgetService;

    std::vector<ProgressBar> m_progressBars;
    std::vector<std::wstring> m_statLabels;
    std::vector<float> m_statValues;
    Table m_processTable;
    Toggle m_pauseOnFullscreenToggle;
    Toggle m_pauseOnBatteryToggle;
    bool m_pauseOnFullscreen = false;

    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_bodyFormat;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_textBrush;

    float m_x=0, m_y=0, m_w=0, m_h=0;
    std::chrono::steady_clock::time_point m_lastRefresh;
    int m_fps = 60;
    float m_frameTime = 0.0f;

    void RefreshStats();
};
