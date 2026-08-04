#pragma once
#include "PageRenderer.h"
#include "Toggle.h"
#include "Slider.h"
#include "Dropdown.h"
#include "Toolbar.h"
#include "Tabs.h"
#include "PropertyGrid.h"
#include "modules/Personalization/interfaces/IPersonalizationService.hpp"
#include <string>
#include <vector>
#include <memory>

class PersonalizationPage : public PageRenderer {
public:
    PersonalizationPage(ID2D1RenderTarget* rt, IDWriteFactory* dwrite,
                        const GlowStyle& style,
                        std::shared_ptr<rex::personalization::IPersonalizationService> svc);
    ~PersonalizationPage() = default;

    void SetBounds(float x, float y, float w, float h);
    void Update(float dt) override;
    void Render(ID2D1RenderTarget* rt) override;
    void HandleClick(float mx, float my) override;

private:
    std::shared_ptr<rex::personalization::IPersonalizationService> m_svc;

    Tabs m_tabs;
    Toolbar m_toolbar;
    PropertyGrid m_props;

    Toggle m_darkMode;
    Toggle m_transparency;
    Toggle m_animations;
    Toggle m_snapLayouts;
    Toggle m_taskbarCenter;
    Toggle m_taskbarAutoHide;
    Toggle m_showSearch;
    Toggle m_showTaskView;
    Toggle m_showWidgets;

    Slider m_accentOpacity;
    Slider m_taskbarOpacity;
    Slider m_fontSize;

    Dropdown m_accentColor;
    Dropdown m_themeMode;
    Dropdown m_taskbarAlign;
    Dropdown m_startLayout;

    float m_x = 0, m_y = 0, m_w = 0, m_h = 0;
    int m_activeSection = 0;
};
