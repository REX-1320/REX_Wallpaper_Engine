#include "ContextMenu.h"

ContextMenu::ContextMenu(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, const GlowStyle& style)
    : m_style(style) {
    if (dwrite) {
        dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-us", &m_format);
    }
    if (rt) {
        rt->CreateSolidColorBrush(D2D1::ColorF(0.12f,0.12f,0.14f,0.95f), &m_bgBrush);
        rt->CreateSolidColorBrush(style.textColor, &m_textBrush);
        rt->CreateSolidColorBrush(D2D1::ColorF(style.glowColor.r, style.glowColor.g,
            style.glowColor.b, 0.12f), &m_hoverBrush);
        rt->CreateSolidColorBrush(D2D1::ColorF(0.3f,0.3f,0.3f,0.5f), &m_sepBrush);
    }
}

void ContextMenu::SetItems(const std::vector<ContextMenuItem>& items) { m_items = items; }

void ContextMenu::ShowAt(float mx, float my) {
    m_x = mx; m_y = my;
    m_visible = true;
    m_hoveredIndex = -1;
}

void ContextMenu::Hide() { m_visible = false; }

void ContextMenu::HandleClick(float mx, float my) {
    if (!m_visible) return;
    float y = m_y;
    for (size_t i = 0; i < m_items.size(); ++i) {
        float itemH = m_items[i].separator ? 8 : 28;
        if (mx >= m_x && mx <= m_x+m_width && my >= y && my <= y+itemH) {
            if (!m_items[i].separator && m_items[i].enabled && m_items[i].action) {
                m_items[i].action();
                m_visible = false;
            }
            return;
        }
        y += itemH;
    }
    m_visible = false;
}

void ContextMenu::Update(float dt) {}

void ContextMenu::Render(ID2D1RenderTarget* rt) {
    if (!m_visible) return;
    float y = m_y;
    for (size_t i = 0; i < m_items.size(); ++i) {
        float itemH = m_items[i].separator ? 8 : 28;
        if (!m_items[i].separator) {
            if ((int)i == m_hoveredIndex) {
                rt->FillRectangle(D2D1::RectF(m_x, y, m_x+m_width, y+itemH), m_hoverBrush.Get());
            }
            m_textBrush->SetColor(D2D1::ColorF(1,1,1,m_items[i].enabled ? 1.0f : 0.4f));
            rt->DrawTextW(m_items[i].label.c_str(), (UINT32)m_items[i].label.size(), m_format.Get(),
                D2D1::RectF(m_x+10, y, m_x+m_width-10, y+itemH), m_textBrush.Get());
        } else {
            rt->DrawLine(D2D1::Point2F(m_x+8, y+4), D2D1::Point2F(m_x+m_width-8, y+4),
                m_sepBrush.Get(), 1);
        }
        y += itemH;
    }
    D2D1_RECT_F bg = {m_x, m_y, m_x+m_width, y};
    rt->FillRoundedRectangle(D2D1::RoundedRect(bg, 8, 8), m_bgBrush.Get());
}
