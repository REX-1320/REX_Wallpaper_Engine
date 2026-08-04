#include "shell/TopBar.h"

namespace rex::shell {

TopBar::TopBar() : m_searchBar(nullptr, nullptr, GlowStyle{}) {}

void TopBar::Init(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, const GlowStyle& style) {
    if (!dwrite || !rt) return;

    m_searchBar = SearchBar(rt, dwrite, style);

    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        14.0f, L"en-us", &m_titleFormat);
    if (m_titleFormat) {
        m_titleFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        m_titleFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.9f), &m_titleBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.08f), &m_edgeBrush);

    m_searchBar.SetPlaceholder(L"Search walls, settings, commands\u2026");
}

void TopBar::SetBounds(float x, float y, float w, float h) {
    m_x = x; m_y = y; m_w = w; m_h = h;
    m_searchBar.SetPosition(60 + 120, 6, (std::min)(600.0f, w - 360.0f));
    m_initialized = true;
}

void TopBar::Update(float dt) {
    m_searchBar.Update(dt);
}

void TopBar::Render(ID2D1RenderTarget* rt) {
    if (!rt) return;

    rt->DrawLine(D2D1::Point2F(m_x, m_y + m_h - 1),
        D2D1::Point2F(m_x + m_w, m_y + m_h - 1),
        m_edgeBrush.Get(), 1.0f);

    float titleX = m_x + 16.0f;
    rt->DrawTextW(L"REX", 3, m_titleFormat.Get(),
        D2D1::RectF(titleX, m_y, titleX + 60, m_y + m_h), m_titleBrush.Get());

    m_searchBar.Render(rt);
}

void TopBar::HandleHover(float mx, float my) {
}

void TopBar::HandleClick(float mx, float my) {
}

}
