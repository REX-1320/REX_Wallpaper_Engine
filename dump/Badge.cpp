#include "Badge.h"

Badge::Badge(const std::wstring& text, ID2D1RenderTarget* rt, IDWriteFactory* dwrite,
             const GlowStyle& style) : m_text(text), m_style(style) {
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"en-us", &m_format);
    m_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    rt->CreateSolidColorBrush(D2D1::ColorF(style.glowColor.r, style.glowColor.g,
        style.glowColor.b, 0.2f), &m_bgBrush);
    rt->CreateSolidColorBrush(style.textColor, &m_textBrush);
}

void Badge::SetPosition(float x, float y) { m_x = x; m_y = y; }

void Badge::Render(ID2D1RenderTarget* rt) {
    float w = m_text.size() * 8.0f + 16;
    D2D1_RECT_F bg = {m_x, m_y, m_x + w, m_y + 20};
    rt->FillRoundedRectangle(D2D1::RoundedRect(bg, 10, 10), m_bgBrush.Get());
    rt->DrawTextW(m_text.c_str(), (UINT32)m_text.size(), m_format.Get(), bg, m_textBrush.Get());
}
