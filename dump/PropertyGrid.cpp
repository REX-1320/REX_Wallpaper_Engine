#include "PropertyGrid.h"

PropertyGrid::PropertyGrid(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, const GlowStyle& style)
    : m_style(style) {
    if (!dwrite || !rt) return;
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-us", &m_labelFormat);
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-us", &m_valueFormat);
    m_valueFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    rt->CreateSolidColorBrush(style.textColor, &m_valueBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.7f,0.7f,0.7f,1.0f), &m_labelBrush);
}

void PropertyGrid::SetBounds(float x, float y, float width) {
    m_x = x; m_y = y; m_width = width;
}

void PropertyGrid::SetProperties(const std::vector<PropertyRow>& props) { m_props = props; }

void PropertyGrid::Render(ID2D1RenderTarget* rt) {
    if (!rt || !m_labelFormat || !m_valueFormat || !m_labelBrush || !m_valueBrush) return;
    float y = m_y;
    for (auto& prop : m_props) {
        rt->DrawTextW(prop.label.c_str(), (UINT32)prop.label.size(), m_labelFormat.Get(),
            D2D1::RectF(m_x, y, m_x + m_width*0.4f, y+24), m_labelBrush.Get());
        rt->DrawTextW(prop.value.c_str(), (UINT32)prop.value.size(), m_valueFormat.Get(),
            D2D1::RectF(m_x + m_width*0.4f, y, m_x + m_width, y+24), m_valueBrush.Get());
        y += 26;
    }
}
