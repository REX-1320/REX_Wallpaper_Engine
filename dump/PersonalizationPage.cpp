#include "PersonalizationPage.h"
#include <d2d1_3.h>
#include <dwrite.h>
#include <wrl/client.h>

PersonalizationPage::PersonalizationPage(ID2D1RenderTarget* rt, IDWriteFactory* dwrite,
                                         const GlowStyle& style,
                                         std::shared_ptr<rex::personalization::IPersonalizationService> svc)
    : PageRenderer(rt, dwrite, style)
    , m_svc(svc)
    , m_tabs(rt, dwrite, style)
    , m_toolbar(rt, dwrite, style)
    , m_props(rt, dwrite, style)
    , m_darkMode(rt, style)
    , m_transparency(rt, style)
    , m_animations(rt, style)
    , m_snapLayouts(rt, style)
    , m_taskbarCenter(rt, style)
    , m_taskbarAutoHide(rt, style)
    , m_showSearch(rt, style)
    , m_showTaskView(rt, style)
    , m_showWidgets(rt, style)
    , m_accentOpacity(rt, dwrite, style, 0, 100, 1)
    , m_taskbarOpacity(rt, dwrite, style, 0, 100, 1)
    , m_fontSize(rt, dwrite, style, 8, 72, 1)
    , m_accentColor(rt, dwrite, style)
    , m_themeMode(rt, dwrite, style)
    , m_taskbarAlign(rt, dwrite, style)
    , m_startLayout(rt, dwrite, style)
{
    m_tabs.SetTabs({L"Appearance", L"Effects", L"Taskbar", L"Explorer", L"Themes"});
    m_themeMode.SetItems({L"Light", L"Dark", L"Custom"});
    m_accentColor.SetItems({L"Blue", L"Green", L"Purple", L"Orange", L"Red"});
    m_taskbarAlign.SetItems({L"Left", L"Center"});
    m_startLayout.SetItems({L"Default", L"More Pins", L"More Recommendations"});

    m_darkMode.SetChecked(true);
    m_showSearch.SetChecked(true);
    m_showTaskView.SetChecked(true);
    m_accentOpacity.SetValue(80);
    m_fontSize.SetValue(10);
}

void PersonalizationPage::SetBounds(float x, float y, float w, float h) {
    m_x = x; m_y = y; m_w = w; m_h = h;
    const float pad = 16;
    const float topArea = 48;

    m_tabs.SetBounds(x + pad, y + pad, w - pad * 2);
    m_toolbar.SetBounds(x + pad, y + pad + 36 + 8, w - pad * 2);

    float contentY = y + topArea;
    float rowH = 36;
    float colX = x + pad;
    float colW = (w - pad * 3) / 2;

    switch (m_activeSection) {
    case 0:
        m_darkMode.SetPosition(colX, contentY); contentY += rowH;
        m_transparency.SetPosition(colX, contentY); contentY += rowH;
        m_animations.SetPosition(colX, contentY); contentY += rowH;
        m_accentOpacity.SetPosition(colX, contentY, colW); contentY += rowH;
        m_accentColor.SetBounds(colX, contentY, colW); contentY += rowH;
        m_themeMode.SetBounds(colX, contentY, colW); contentY += rowH;
        m_fontSize.SetPosition(colX, contentY, colW); contentY += rowH;
        break;
    case 1:
        m_snapLayouts.SetPosition(colX, contentY); contentY += rowH;
        break;
    case 2:
        m_taskbarCenter.SetPosition(colX, contentY); contentY += rowH;
        m_taskbarAutoHide.SetPosition(colX, contentY); contentY += rowH;
        m_taskbarOpacity.SetPosition(colX, contentY, colW); contentY += rowH;
        m_taskbarAlign.SetBounds(colX, contentY, colW); contentY += rowH;
        break;
    case 3:
        m_showSearch.SetPosition(colX, contentY); contentY += rowH;
        m_showTaskView.SetPosition(colX, contentY); contentY += rowH;
        m_showWidgets.SetPosition(colX, contentY); contentY += rowH;
        m_startLayout.SetBounds(colX, contentY, colW); contentY += rowH;
        break;
    case 4:
        m_props.SetBounds(colX, contentY, colW);
        break;
    }
}

void PersonalizationPage::Update(float dt) {
    m_tabs.Update(dt);
    m_toolbar.Update(dt);
    m_darkMode.Update(dt);
    m_transparency.Update(dt);
    m_animations.Update(dt);
    m_snapLayouts.Update(dt);
    m_taskbarCenter.Update(dt);
    m_taskbarAutoHide.Update(dt);
    m_showSearch.Update(dt);
    m_showTaskView.Update(dt);
    m_showWidgets.Update(dt);
    m_accentOpacity.Update(dt);
    m_taskbarOpacity.Update(dt);
    m_fontSize.Update(dt);
}

void PersonalizationPage::Render(ID2D1RenderTarget* rt) {
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> bgBrush;
    rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.85f), &bgBrush);
    if (bgBrush) {
        D2D1_RECT_F bgRect = { m_x, m_y, m_x + m_w, m_y + m_h };
        rt->FillRectangle(&bgRect, bgBrush.Get());
    }

    m_tabs.Render(rt);
    m_toolbar.Render(rt);

    switch (m_activeSection) {
    case 0:
        m_darkMode.Render(rt);
        m_transparency.Render(rt);
        m_animations.Render(rt);
        m_accentOpacity.Render(rt);
        m_accentColor.Render(rt);
        m_themeMode.Render(rt);
        m_fontSize.Render(rt);
        break;
    case 1:
        m_snapLayouts.Render(rt);
        break;
    case 2:
        m_taskbarCenter.Render(rt);
        m_taskbarAutoHide.Render(rt);
        m_taskbarOpacity.Render(rt);
        m_taskbarAlign.Render(rt);
        break;
    case 3:
        m_showSearch.Render(rt);
        m_showTaskView.Render(rt);
        m_showWidgets.Render(rt);
        m_startLayout.Render(rt);
        break;
    case 4:
        m_props.Render(rt);
        break;
    }
}

void PersonalizationPage::HandleClick(float mx, float my) {
    if (m_tabs.HitTest(mx, my)) {
        int prev = m_activeSection;
        m_activeSection = m_tabs.GetSelected();
        if (m_activeSection != prev) {
            SetBounds(m_x, m_y, m_w, m_h);
        }
        return;
    }
    if (m_toolbar.HitTest(mx, my)) return;

    switch (m_activeSection) {
    case 0:
        m_darkMode.HitTest(mx, my);
        m_accentColor.HandleClick(mx, my);
        m_themeMode.HandleClick(mx, my);
        break;
    case 2:
        m_taskbarCenter.HitTest(mx, my);
        m_taskbarAutoHide.HitTest(mx, my);
        m_taskbarAlign.HandleClick(mx, my);
        break;
    case 3:
        m_startLayout.HandleClick(mx, my);
        m_showSearch.HitTest(mx, my);
        m_showTaskView.HitTest(mx, my);
        m_showWidgets.HitTest(mx, my);
        break;
    }
}
