#include "shell/Sidebar.h"
#include <algorithm>

namespace rex::shell {

Sidebar::Sidebar() : m_buttons{}, m_itemToFlatIdx{} {}

void Sidebar::Init(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, const GlowStyle& style) {
    if (!dwrite || !rt) return;

    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        12.0f, L"en-us", &m_headerFormat);

    rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.06f), &m_edgeBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.4f, 0.6f, 1.0f, 1.0f), &m_accentBrush);

    m_groups = {
        { L"HOME",        { L"Dashboard" }},
        { L"DESKTOP",     { L"Wallpapers", L"Widgets", L"Themes", L"Personalization" }},
        { L"DISCOVER",    { L"Marketplace", L"Downloads" }},
        { L"SYSTEM",      { L"Performance", L"Activity", L"Settings" }},
        { L"ADVANCED",    { L"Developer", L"About" }}
    };

    BuildItems();
    m_initialized = true;
}

void Sidebar::BuildItems() {
    m_buttons.clear();
    m_itemToFlatIdx.clear();

    for (auto& group : m_groups) {
        if (!group.header.empty()) {
            GlowTextButton hdr(group.header, nullptr, nullptr, GlowStyle{});
            hdr.SetFontSize(11.0f);
            m_buttons.push_back(std::move(hdr));
            m_itemToFlatIdx.push_back(-1);
        }
        for (auto& item : group.items) {
            GlowTextButton btn(item, nullptr, nullptr, GlowStyle{});
            btn.SetFontSize(15.0f);
            m_buttons.push_back(std::move(btn));
            m_itemToFlatIdx.push_back((int)m_buttons.size() - 1);
        }
    }
}

void Sidebar::SetBounds(float x, float y, float w, float h) {
    m_x = x; m_y = y; m_w = w; m_h = h;
    float cy = m_y + 12.0f;
    int flatIdx = 0;
    for (auto& group : m_groups) {
        if (!group.header.empty()) {
            if (flatIdx < (int)m_buttons.size())
                m_buttons[flatIdx].SetPosition(m_x + 16, cy);
            cy += 22.0f;
            flatIdx++;
        }
        for (size_t i = 0; i < group.items.size(); ++i) {
            if (flatIdx < (int)m_buttons.size())
                m_buttons[flatIdx].SetPosition(m_x + 16, cy);
            cy += 38.0f;
            flatIdx++;
        }
        cy += 8.0f;
    }
}

void Sidebar::Update(float dt) {
    for (auto& btn : m_buttons) btn.Update(dt);
}

void Sidebar::Render(ID2D1RenderTarget* rt) {
    if (!rt) return;

    rt->DrawLine(D2D1::Point2F(m_x + m_w - 1, m_y),
        D2D1::Point2F(m_x + m_w - 1, m_y + m_h),
        m_edgeBrush.Get(), 1.0f);

    int flatIdx = 0;
    for (auto& group : m_groups) {
        if (!group.header.empty() && flatIdx < (int)m_buttons.size()) {
            auto& btn = m_buttons[flatIdx];
            btn.Render(rt);
            flatIdx++;
        }
        for (size_t i = 0; i < group.items.size(); ++i) {
            if (flatIdx < (int)m_buttons.size()) {
                auto& btn = m_buttons[flatIdx];
                int itemIdx = (int)i;
                if (itemIdx == m_activePage) {
                    float by = btn.GetY();
                    rt->FillRectangle(D2D1::RectF(m_x, by, m_x + 3, by + 32),
                        m_accentBrush.Get());
                }
                btn.Render(rt);
            }
            flatIdx++;
        }
    }
}

void Sidebar::HandleHover(float mx, float my) {
    for (auto& btn : m_buttons) btn.SetHovered(btn.HitTest(mx, my));
}

void Sidebar::HandleClick(float mx, float my) {
    int flatIdx = 0;
    for (size_t g = 0; g < m_groups.size(); ++g) {
        flatIdx++;
        for (size_t i = 0; i < m_groups[g].items.size(); ++i) {
            if (flatIdx < (int)m_buttons.size() && m_buttons[flatIdx].HitTest(mx, my)) {
                SetActivePage((int)i);
                return;
            }
            flatIdx++;
        }
    }
}

void Sidebar::SetHoveredItem(float mx, float my) {
    m_hoveredItem = -1;
    float cy = m_y + 12.0f;
    for (size_t g = 0; g < m_groups.size(); ++g) {
        cy += 22.0f;
        for (size_t i = 0; i < m_groups[g].items.size(); ++i) {
            float itemH = 38.0f;
            if (my >= cy && my <= cy + itemH) { m_hoveredItem = (int)i; return; }
            cy += itemH;
        }
        cy += 8.0f;
    }
}

void Sidebar::SetActivePage(int page) {
    m_activePage = page;
    UpdateSelection();
}

void Sidebar::UpdateSelection() {
    int flatIdx = 0;
    for (size_t g = 0; g < m_groups.size(); ++g) {
        flatIdx++;
        for (size_t i = 0; i < m_groups[g].items.size(); ++i) {
            if (flatIdx < (int)m_buttons.size()) {
                m_buttons[flatIdx].SetSelected((int)i == m_activePage);
            }
            flatIdx++;
        }
    }
}

}
