#include "GlowTextButton.h"

GlowTextButton::GlowTextButton(const std::wstring& text, ID2D1RenderTarget* rt,
                               IDWriteFactory* dwriteFactory, const GlowStyle& style)
    : m_text(text), m_style(style),
      m_selectSpring(300.0f, 25.0f, 1.0f, 0.0f),
      m_scaleSpring(400.0f, 30.0f, 1.0f, 1.0f),
      m_hoverSpring(250.0f, 20.0f, 1.0f, 0.0f),
      m_dwriteFactory(dwriteFactory)
{
    if (!dwriteFactory || !rt) return;
    dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        m_fontSize, L"en-us", &m_textFormat);
    CreateTextLayout(dwriteFactory);
    rt->CreateSolidColorBrush(style.textColor, &m_textBrush);
    rt->CreateSolidColorBrush(style.glowColor, &m_glowBrush);
}

void GlowTextButton::SetFontSize(float size) {
    if (m_fontSize == size) return;
    m_fontSize = size;
    if (m_dwriteFactory) {
        m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            m_fontSize, L"en-us", &m_textFormat);
        CreateTextLayout(m_dwriteFactory.Get());
    }
}

void GlowTextButton::SetSelected(bool selected) {
    m_selected = selected;
    m_selectSpring.SetTarget(selected ? 1.0f : 0.0f);
    m_scaleSpring.SetTarget(selected ? 1.05f : 1.0f);
}

void GlowTextButton::SetHovered(bool hovered) {
    m_hovered = hovered;
    m_hoverSpring.SetTarget(hovered ? 1.0f : 0.0f);
    m_scaleSpring.SetTarget(hovered ? 1.08f : (m_selected ? 1.05f : 1.0f));
}

bool GlowTextButton::HitTest(float x, float y) {
    if (!m_textLayout) return false;
    DWRITE_TEXT_METRICS metrics;
    m_textLayout->GetMetrics(&metrics);
    float left = m_x, top = m_y;
    float right = left + metrics.width;
    float bottom = top + metrics.height;
    return x >= left && x <= right && y >= top && y <= bottom;
}

void GlowTextButton::Update(float deltaTime) {
    m_selectSpring.Update(deltaTime);
    m_scaleSpring.Update(deltaTime);
    m_hoverSpring.Update(deltaTime);
}

void GlowTextButton::Render(ID2D1RenderTarget* rt) {
    if (!rt || !m_textLayout || !m_textBrush || !m_glowBrush) return;
    float glowIntensity = m_selectSpring.GetValue() * 0.8f + m_hoverSpring.GetValue() * 0.6f;
    if (glowIntensity > 1.0f) glowIntensity = 1.0f;
    float scale = m_scaleSpring.GetValue();

    if (glowIntensity > 0.01f) {
        rt->SetTransform(D2D1::Matrix3x2F::Scale(scale * 1.1f, scale * 1.1f,
                                                  D2D1::Point2F(m_x, m_y)));
        D2D1::ColorF glow = m_style.glowColor;
        glow.a = glowIntensity * 0.3f;
        m_glowBrush->SetColor(glow);
        rt->DrawTextLayout(D2D1::Point2F(m_x, m_y), m_textLayout.Get(), m_glowBrush.Get());

        glow.a = glowIntensity * 0.5f;
        m_glowBrush->SetColor(glow);
        rt->SetTransform(D2D1::Matrix3x2F::Scale(scale * 1.05f, scale * 1.05f,
                                                  D2D1::Point2F(m_x, m_y)));
        rt->DrawTextLayout(D2D1::Point2F(m_x, m_y), m_textLayout.Get(), m_glowBrush.Get());
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
    }

    rt->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale, D2D1::Point2F(m_x, m_y)));
    rt->DrawTextLayout(D2D1::Point2F(m_x, m_y), m_textLayout.Get(), m_textBrush.Get());
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
}

void GlowTextButton::CreateTextLayout(IDWriteFactory* dwriteFactory) {
    if (m_text.empty() || !dwriteFactory) return;
    dwriteFactory->CreateTextLayout(m_text.c_str(), (UINT32)m_text.size(),
        m_textFormat.Get(), FLT_MAX, FLT_MAX, &m_textLayout);
}
