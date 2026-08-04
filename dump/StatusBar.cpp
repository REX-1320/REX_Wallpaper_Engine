#include "StatusBar.h"

StatusBar::StatusBar(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, const GlowStyle& style)
    : m_progressBar(rt, style), m_style(style) {
    if (dwrite) {
        dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"en-us", &m_textFormat);
    }
    if (rt) {
        rt->CreateSolidColorBrush(D2D1::ColorF(0.08f,0.08f,0.1f,0.9f), &m_bgBrush);
        rt->CreateSolidColorBrush(D2D1::ColorF(0.7f,0.7f,0.7f,1.0f), &m_textBrush);
    }
}

void StatusBar::SetBounds(float x, float y, float width) {
    m_x = x; m_y = y; m_width = width;
    m_progressBar.SetBounds(x, y + m_height - 4, width, 2);
}

void StatusBar::SetLeftItems(const std::vector<StatusItem>& items) { m_leftItems = items; }
void StatusBar::SetRightItems(const std::vector<StatusItem>& items) { m_rightItems = items; }
void StatusBar::SetProgress(float p) { m_progressBar.SetProgress(p); }

void StatusBar::Render(ID2D1RenderTarget* rt) {
    D2D1_RECT_F bar = {m_x, m_y, m_x+m_width, m_y+m_height};
    rt->FillRectangle(bar, m_bgBrush.Get());

    float x = m_x + 8;
    for (auto& item : m_leftItems) {
        D2D1_RECT_F rect = {x, m_y, x+100, m_y+m_height};
        rt->DrawTextW(item.text.c_str(), (UINT32)item.text.size(), m_textFormat.Get(),
            rect, m_textBrush.Get());
        x += item.text.size() * 7.0f + 16;
    }

    float rx = m_x + m_width - 8;
    for (auto it = m_rightItems.rbegin(); it != m_rightItems.rend(); ++it) {
        rx -= it->text.size() * 7.0f + 16;
        D2D1_RECT_F rect = {rx, m_y, rx+100, m_y+m_height};
        rt->DrawTextW(it->text.c_str(), (UINT32)it->text.size(), m_textFormat.Get(),
            rect, m_textBrush.Get());
    }
    m_progressBar.Render(rt);
}
