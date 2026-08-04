#include "Card.h"
#include "graphics/effects/LiquidGlassBrush.hpp"

Card::Card(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, const GlowStyle& style)
    : m_style(style), m_dwrite(dwrite), m_rt(rt)
{
    if (!dwrite || !rt) return;
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        16.0f, L"en-us", &m_titleFormat);
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        13.0f, L"en-us", &m_subtitleFormat);

    rt->CreateSolidColorBrush(D2D1::ColorF(1,1,1,0.08f), &m_bgBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(style.glowColor.r, style.glowColor.g,
        style.glowColor.b, 0.3f), &m_borderBrush);
    rt->CreateSolidColorBrush(style.textColor, &m_textBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.7f,0.7f,0.7f,1.0f), &m_subtitleBrush);
}

void Card::SetBounds(float x, float y, float w, float h) {
    m_x = x; m_y = y; m_width = w; m_height = h;
    m_roundedRect = nullptr;
}

void Card::SetTitle(const std::wstring& title) {
    m_title = title;
    if (m_dwrite && !title.empty()) {
        m_dwrite->CreateTextLayout(title.c_str(), (UINT32)title.size(),
            m_titleFormat.Get(), m_width - 20, 24, &m_titleLayout);
    }
}

void Card::SetSubtitle(const std::wstring& subtitle) {
    m_subtitle = subtitle;
    if (m_dwrite && !subtitle.empty()) {
        m_dwrite->CreateTextLayout(subtitle.c_str(), (UINT32)subtitle.size(),
            m_subtitleFormat.Get(), m_width - 20, 20, &m_subtitleLayout);
    }
}

void Card::AddAction(const std::wstring& label, std::function<void()> cb) {
    Button btn(label, m_rt, m_dwrite, m_style);
    btn.SetOnClick(std::move(cb));
    m_actionButtons.push_back(std::move(btn));
}

bool Card::HitTest(float mx, float my) {
    return mx >= m_x && mx <= m_x + m_width && my >= m_y && my <= m_y + m_height;
}

void Card::Update(float dt) {
    m_elevationSpring.Update(dt);
    for (auto& btn : m_actionButtons) btn.Update(dt);
}

void Card::Render(ID2D1RenderTarget* rt) {
    if (!rt) return;
    float elevation = m_elevationSpring.GetValue();

    D2D1_ROUNDED_RECT glassRect = D2D1::RoundedRect(
        D2D1::RectF(m_x, m_y - elevation * m_glassTokens.motion.hoverLiftPx, m_x + m_width, m_y + m_height - elevation * m_glassTokens.motion.hoverLiftPx),
        16.0f, 16.0f // Spec §6.1: 16px corner radius
    );

    rex::graphics::LiquidGlassBrush::RenderGlassCard(rt, glassRect, m_glassTokens, elevation);

    if (m_titleLayout) {
        rt->DrawTextLayout(D2D1::Point2F(m_x+10, m_y+8), m_titleLayout.Get(), m_textBrush.Get());
    }
    if (m_subtitleLayout) {
        rt->DrawTextLayout(D2D1::Point2F(m_x+10, m_y+30), m_subtitleLayout.Get(), m_subtitleBrush.Get());
    }
    if (m_showImage) {
        D2D1_RECT_F imgRect = {m_x+10, m_y+50, m_x+m_width-10, m_y+m_height-50};
        float imgAlpha = 0.08f + elevation * 0.02f;
        m_bgBrush->SetColor(D2D1::ColorF(1,1,1,imgAlpha));
        rt->FillRectangle(&imgRect, m_bgBrush.Get());
    }
    for (auto& btn : m_actionButtons) btn.Render(rt);
}
