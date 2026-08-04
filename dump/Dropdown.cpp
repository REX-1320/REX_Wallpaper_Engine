#include "Dropdown.h"

Dropdown::Dropdown(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, const GlowStyle& style)
    : m_style(style) {
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-us", &m_format);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.15f,0.15f,0.17f,0.9f), &m_bgBrush);
    rt->CreateSolidColorBrush(style.textColor, &m_textBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.2f,0.2f,0.22f,0.95f), &m_itemBrush);
}

void Dropdown::SetBounds(float x, float y, float width) { m_x = x; m_y = y; m_width = width; }
void Dropdown::SetItems(const std::vector<std::wstring>& items) { m_items = items; }
void Dropdown::SetSelected(int index) { m_selectedIndex = index; }

bool Dropdown::HitTest(float mx, float my) {
    if (mx >= m_x && mx <= m_x+m_width && my >= m_y && my <= m_y+m_height) return true;
    if (m_open) {
        for (size_t i = 0; i < m_items.size(); ++i) {
            float iy = m_y + m_height + i * m_height;
            if (mx >= m_x && mx <= m_x+m_width && my >= iy && my <= iy+m_height) return true;
        }
    }
    return false;
}

void Dropdown::HandleClick(float mx, float my) {
    if (mx >= m_x && mx <= m_x+m_width && my >= m_y && my <= m_y+m_height) {
        m_open = !m_open;
        return;
    }
    if (m_open) {
        for (size_t i = 0; i < m_items.size(); ++i) {
            float iy = m_y + m_height + i * m_height;
            if (mx >= m_x && mx <= m_x+m_width && my >= iy && my <= iy+m_height) {
                m_selectedIndex = (int)i;
                m_open = false;
                if (m_onChange) m_onChange((int)i);
                return;
            }
        }
        m_open = false;
    }
}

void Dropdown::Render(ID2D1RenderTarget* rt) {
    D2D1_RECT_F bg = {m_x, m_y, m_x+m_width, m_y+m_height};
    rt->FillRoundedRectangle(D2D1::RoundedRect(bg, 6, 6), m_bgBrush.Get());
    std::wstring display = m_selectedIndex >= 0 && m_selectedIndex < (int)m_items.size()
        ? m_items[m_selectedIndex] : L"";
    rt->DrawTextW(display.c_str(), (UINT32)display.size(), m_format.Get(),
        D2D1::RectF(m_x+8, m_y, m_x+m_width-24, m_y+m_height), m_textBrush.Get());
    rt->DrawTextW(L"\u25BC", 1, m_format.Get(),
        D2D1::RectF(m_x+m_width-20, m_y, m_x+m_width, m_y+m_height), m_textBrush.Get());

    if (m_open) {
        for (size_t i = 0; i < m_items.size(); ++i) {
            float iy = m_y + m_height + i * m_height;
            D2D1_RECT_F itemBg = {m_x, iy, m_x+m_width, iy+m_height};
            rt->FillRoundedRectangle(D2D1::RoundedRect(itemBg, 0, 0), m_itemBrush.Get());
            rt->DrawTextW(m_items[i].c_str(), (UINT32)m_items[i].size(), m_format.Get(),
                D2D1::RectF(m_x+8, iy, m_x+m_width, iy+m_height), m_textBrush.Get());
        }
    }
}
