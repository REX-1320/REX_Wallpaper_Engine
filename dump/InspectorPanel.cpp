#include "shell/InspectorPanel.h"

namespace rex::shell {

InspectorPanel::InspectorPanel() : m_propertyGrid(nullptr, nullptr, GlowStyle{}) {}

void InspectorPanel::Init(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, const GlowStyle& style) {
    if (!dwrite || !rt) return;

    m_propertyGrid = PropertyGrid(rt, dwrite, style);

    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        13.0f, L"en-us", &m_headerFormat);
    rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.7f), &m_headerBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.08f), &m_edgeBrush);
}

void InspectorPanel::SetBounds(float x, float y, float w, float h) {
    m_x = x; m_y = y; m_w = w; m_h = h;
    m_propertyGrid.SetBounds(m_x + 8, m_y + 36, m_w - 16);
}

void InspectorPanel::Update(float dt) {
}

void InspectorPanel::Render(ID2D1RenderTarget* rt) {
    if (!rt || m_w < 10) return;

    rt->DrawLine(D2D1::Point2F(m_x, m_y), D2D1::Point2F(m_x, m_y + m_h), m_edgeBrush.Get(), 1.0f);
    rt->DrawTextW(L"Inspector", 9, m_headerFormat.Get(),
        D2D1::RectF(m_x + 8, m_y + 8, m_x + m_w - 8, m_y + 32), m_headerBrush.Get());
    m_propertyGrid.Render(rt);
}

void InspectorPanel::HandleClick(float mx, float my) {
}

}
