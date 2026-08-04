#include "Tooltip.h"

Tooltip::Tooltip(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, const GlowStyle& style)
    : m_style(style) {
    if (dwrite) {
        dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"en-us", &m_format);
    }
    if (rt) {
        rt->CreateSolidColorBrush(D2D1::ColorF(0.1f,0.1f,0.12f,0.9f), &m_bgBrush);
        rt->CreateSolidColorBrush(style.textColor, &m_textBrush);
    }
}

void Tooltip::ShowAt(const std::wstring& text, float mx, float my) {
    m_text = text;
    m_x = mx + 12;
    m_y = my + 12;
    m_visible = true;
    m_timer = 0;
}

void Tooltip::Hide() { m_visible = false; }

void Tooltip::Update(float dt) {
    if (m_visible) m_timer += dt;
}

void Tooltip::Render(ID2D1RenderTarget* rt) {
    if (!m_visible) return;
    float w = m_text.size() * 6.0f + 16;
    D2D1_RECT_F bg = {m_x, m_y, m_x+w, m_y+24};
    rt->FillRoundedRectangle(D2D1::RoundedRect(bg, 6, 6), m_bgBrush.Get());
    rt->DrawTextW(m_text.c_str(), (UINT32)m_text.size(), m_format.Get(),
        D2D1::RectF(m_x+6, m_y, m_x+w, m_y+24), m_textBrush.Get());
}
