#include "SectionHeader.h"

SectionHeader::SectionHeader(const std::wstring& title, ID2D1RenderTarget* rt,
                             IDWriteFactory* dwrite, const GlowStyle& style,
                             const std::wstring& actionLabel, std::function<void()> actionCb)
    : m_title(title), m_actionLabel(actionLabel), m_actionCb(std::move(actionCb)), m_style(style)
{
    if (!dwrite || !rt) return;
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 22.0f, L"en-us", &m_titleFormat);
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"en-us", &m_actionFormat);
    dwrite->CreateTextLayout(title.c_str(), (UINT32)title.size(), m_titleFormat.Get(),
        FLT_MAX, 30.0f, &m_titleLayout);
    if (!m_actionLabel.empty()) {
        dwrite->CreateTextLayout(actionLabel.c_str(), (UINT32)actionLabel.size(),
            m_actionFormat.Get(), FLT_MAX, 20.0f, &m_actionLayout);
    }
    rt->CreateSolidColorBrush(style.textColor, &m_titleBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(style.glowColor.r, style.glowColor.g,
        style.glowColor.b, 0.8f), &m_actionBrush);
}

void SectionHeader::Render(ID2D1RenderTarget* rt) {
    if (!rt || !m_titleLayout) return;
    rt->DrawTextLayout(D2D1::Point2F(m_x, m_y), m_titleLayout.Get(), m_titleBrush.Get());
    if (m_actionLayout) {
        DWRITE_TEXT_METRICS tm;
        m_titleLayout->GetMetrics(&tm);
        float actionX = m_x + tm.width + 20;
        float actionY = m_y + 6;
        rt->DrawTextLayout(D2D1::Point2F(actionX, actionY), m_actionLayout.Get(), m_actionBrush.Get());
    }
}

bool SectionHeader::HitTestAction(float mx, float my) {
    return false;
}
