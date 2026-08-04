#include "Tabs.h"

Tabs::Tabs(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, const GlowStyle& style)
    : m_style(style) {
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-us", &m_textFormat);
    m_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.7f,0.7f,0.7f,1.0f), &m_textBrush);
    rt->CreateSolidColorBrush(style.textColor, &m_activeBrush);
    rt->CreateSolidColorBrush(style.glowColor, &m_indicatorBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(style.glowColor.r, style.glowColor.g,
        style.glowColor.b, 0.08f), &m_hoverBrush);
}

void Tabs::SetBounds(float x, float y, float width) { m_x = x; m_y = y; m_width = width; }

void Tabs::SetTabs(const std::vector<std::wstring>& labels) {
    m_labels = labels;
    m_tabPositions.clear();
    m_tabWidths.clear();
    float x = m_x;
    for (auto& label : labels) {
        float w = label.size() * 10.0f + 32.0f;
        m_tabWidths.push_back(w);
        m_tabPositions.push_back(x);
        x += w;
    }
}

void Tabs::SetSelected(int index) {
    if (index < 0 || index >= (int)m_labels.size()) return;
    m_selectedIndex = index;
    m_indicatorX.SetTarget(m_tabPositions[index]);
    m_indicatorWidth.SetTarget(m_tabWidths[index]);
    if (m_onChange) m_onChange(index);
}

bool Tabs::HitTest(float mx, float my) {
    if (my < m_y || my > m_y + m_tabHeight) return false;
    for (size_t i = 0; i < m_labels.size(); ++i) {
        if (mx >= m_tabPositions[i] && mx <= m_tabPositions[i] + m_tabWidths[i])
            return true;
    }
    return false;
}

void Tabs::Update(float dt) {
    m_indicatorX.Update(dt);
    m_indicatorWidth.Update(dt);
}

void Tabs::Render(ID2D1RenderTarget* rt) {
    for (size_t i = 0; i < m_labels.size(); ++i) {
        float tx = m_tabPositions[i];
        float tw = m_tabWidths[i];
        D2D1_RECT_F tabRect = {tx, m_y, tx+tw, m_y+m_tabHeight};

        if ((int)i == m_hoveredIndex) {
            rt->FillRoundedRectangle(D2D1::RoundedRect(
                D2D1_RECT_F{tx+2, m_y+2, tx+tw-2, m_y+m_tabHeight-2}, 6, 6), m_hoverBrush.Get());
        }

        auto& brush = (i == m_selectedIndex) ? m_activeBrush : m_textBrush;
        rt->DrawTextW(m_labels[i].c_str(), (UINT32)m_labels[i].size(), m_textFormat.Get(),
            tabRect, brush.Get());
    }

    float ix = m_indicatorX.GetValue();
    float iw = m_indicatorWidth.GetValue();
    D2D1_RECT_F indicator = {ix + 4, m_y + m_tabHeight - 3, ix + iw - 4, m_y + m_tabHeight};
    rt->FillRoundedRectangle(D2D1::RoundedRect(indicator, 1.5f, 1.5f), m_indicatorBrush.Get());
}
