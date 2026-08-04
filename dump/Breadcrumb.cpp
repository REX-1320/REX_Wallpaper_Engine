#include "Breadcrumb.h"

Breadcrumb::Breadcrumb(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, const GlowStyle& style)
    : m_style(style) {
    if (dwrite) {
        dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-us", &m_textFormat);
    }
    if (rt) {
        rt->CreateSolidColorBrush(D2D1::ColorF(0.6f,0.6f,0.6f,1.0f), &m_textBrush);
        rt->CreateSolidColorBrush(style.textColor, &m_activeBrush);
        rt->CreateSolidColorBrush(D2D1::ColorF(0.4f,0.4f,0.4f,1.0f), &m_separatorBrush);
    }
}

void Breadcrumb::SetBounds(float x, float y) { m_x = x; m_y = y; }

void Breadcrumb::SetPath(const std::vector<BreadcrumbItem>& items) {
    m_items = items;
    m_itemWidths.clear();
    for (auto& item : items) {
        m_itemWidths.push_back(item.label.size() * 8.0f + 16);
    }
}

bool Breadcrumb::HitTest(float mx, float my) {
    float x = m_x;
    for (size_t i = 0; i < m_items.size(); ++i) {
        if (mx >= x && mx <= x + m_itemWidths[i] && my >= m_y && my <= m_y + 22) {
            m_hoveredIndex = (int)i;
            return true;
        }
        x += m_itemWidths[i] + 20;
    }
    m_hoveredIndex = -1;
    return false;
}

void Breadcrumb::Render(ID2D1RenderTarget* rt) {
    float x = m_x;
    for (size_t i = 0; i < m_items.size(); ++i) {
        bool isLast = (i == m_items.size() - 1);
        auto& brush = ((int)i == m_hoveredIndex || isLast) ? m_activeBrush : m_textBrush;

        D2D1_RECT_F rect = {x, m_y, x + m_itemWidths[i], m_y + 22};
        rt->DrawTextW(m_items[i].label.c_str(), (UINT32)m_items[i].label.size(),
            m_textFormat.Get(), rect, brush.Get());

        x += m_itemWidths[i];
        if (!isLast) {
            D2D1_RECT_F sepRect = {x, m_y, x + 20, m_y + 22};
            rt->DrawTextW(L" \u203A ", 3, m_textFormat.Get(),
                sepRect, m_separatorBrush.Get());
            x += 20;
        }
    }
}
