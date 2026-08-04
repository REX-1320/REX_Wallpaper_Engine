#include "Gallery.h"
#include <algorithm>

Gallery::Gallery(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, const GlowStyle& style,
                 int columns, float cardWidth, float cardHeight, float gap)
    : m_rt(rt), m_dwrite(dwrite), m_style(style),
      m_columns(columns), m_cardWidth(cardWidth), m_cardHeight(cardHeight), m_gap(gap) {}

void Gallery::SetBounds(float x, float y, float width, float height) {
    m_x = x; m_y = y; m_width = width; m_height = height;
    RebuildCards();
}

void Gallery::SetItems(const std::vector<GalleryItem>& items) {
    m_items = items;
    RebuildCards();
}

void Gallery::RebuildCards() {
    int rows = (int)std::ceil((float)m_items.size() / m_columns);
    m_maxScroll = std::max(0.0f, rows * (m_cardHeight + m_gap) - m_height);
    m_cards.clear();
    for (size_t i = 0; i < m_items.size(); ++i) {
        int col = (int)(i % m_columns);
        int row = (int)(i / m_columns);
        float cx = m_x + col * (m_cardWidth + m_gap);
        float cy = m_y + row * (m_cardHeight + m_gap) - m_scrollOffset;
        Card card(m_rt, m_dwrite, m_style);
        card.SetBounds(cx, cy, m_cardWidth, m_cardHeight);
        card.SetTitle(m_items[i].title);
        card.SetSubtitle(m_items[i].subtitle);
        m_cards.push_back(std::move(card));
    }
}

void Gallery::ScrollBy(float deltaY) {
    m_scrollOffset = std::clamp(m_scrollOffset + deltaY, 0.0f, m_maxScroll);
    m_scrollSpring.SetTarget(m_scrollOffset);
    RebuildCards();
}

void Gallery::ScrollTo(float offsetY) {
    m_scrollOffset = std::clamp(offsetY, 0.0f, m_maxScroll);
    m_scrollSpring.SetTarget(m_scrollOffset);
    RebuildCards();
}

bool Gallery::HitTest(float mx, float my) {
    return mx >= m_x && mx <= m_x + m_width && my >= m_y && my <= m_y + m_height;
}

void Gallery::Update(float dt) {
    m_scrollSpring.Update(dt);
    m_scrollOffset = m_scrollSpring.GetValue();
    RebuildCards();

    m_hoveredIndex = -1;
    for (size_t i = 0; i < m_cards.size(); ++i) {
        m_cards[i].Update(dt);
    }
}

void Gallery::Render(ID2D1RenderTarget* rt) {
    if (!rt) return;
    rt->PushAxisAlignedClip(D2D1::RectF(m_x, m_y, m_x + m_width, m_y + m_height),
        D2D1_ANTIALIAS_MODE_ALIASED);

    for (size_t i = 0; i < m_cards.size(); ++i) {
        m_cards[i].SetHovered(i == m_hoveredIndex);
        m_cards[i].Render(rt);
    }

    rt->PopAxisAlignedClip();
}
