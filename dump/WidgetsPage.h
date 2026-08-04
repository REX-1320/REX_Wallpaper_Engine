#pragma once
#include "PageRenderer.h"
#include "Gallery.h"
#include "PropertyGrid.h"
#include "Toolbar.h"
#include "Dropdown.h"
#include "GlowStyle.h"
#include "modules/Widgets/interfaces/IWidgetService.hpp"
#include <memory>
#include <vector>

class WidgetsPage : public PageRenderer {
public:
    WidgetsPage(ID2D1RenderTarget* rt, IDWriteFactory* dwrite,
                const GlowStyle& style,
                std::shared_ptr<rex::widgets::IWidgetService> widgetService);

    void SetBounds(float x, float y, float w, float h);
    void Update(float dt) override;
    void Render(ID2D1RenderTarget* rt) override;
    void HandleClick(float mx, float my) override;

private:
    std::shared_ptr<rex::widgets::IWidgetService> m_service;
    Toolbar m_toolbar;
    Gallery m_gallery;
    PropertyGrid m_inspector;
    Dropdown m_typeDropdown;

    std::vector<GalleryItem> m_widgetItems;
    int m_selectedIndex = -1;
    float m_x=0, m_y=0, m_w=0, m_h=0;

    void RefreshWidgetList();
    void OnWidgetSelected(int index);
    void RefreshInspector();
};
