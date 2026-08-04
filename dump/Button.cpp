#include "Button.h"

Button::Button(const std::wstring& text, ID2D1RenderTarget* rt,
               IDWriteFactory* dwrite, const GlowStyle& style, const std::wstring& icon)
    : m_text(text), m_icon(icon), m_style(style)
{
    if (!dwrite || !rt) return;
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        14.0f, L"en-us", &m_textFormat);
    m_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    m_textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    dwrite->CreateTextLayout(text.c_str(), (UINT32)text.size(), m_textFormat.Get(),
        m_width, m_height, &m_textLayout);

    rt->CreateSolidColorBrush(style.textColor, &m_textBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(style.glowColor.r, style.glowColor.g,
        style.glowColor.b, 0.15f), &m_bgBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(style.glowColor.r, style.glowColor.g,
        style.glowColor.b, 0.5f), &m_borderBrush);

    m_roundedRect = nullptr;
}

bool Button::HitTest(float mx, float my) {
    return mx >= m_x && mx <= m_x + m_width && my >= m_y && my <= m_y + m_height;
}

void Button::Update(float dt) {
    m_hoverSpring.Update(dt);
    m_pressSpring.Update(dt);
    m_scaleSpring.Update(dt);
    float target = m_pressSpring.GetTarget() > 0.5f ? 0.95f :
                   m_hoverSpring.GetTarget() > 0.5f ? 1.05f : 1.0f;
    m_scaleSpring.SetTarget(target);
}

void Button::Render(ID2D1RenderTarget* rt) {
    if (!rt || !m_textLayout) return;

    float scale = m_scaleSpring.GetValue();
    float glowAlpha = m_hoverSpring.GetValue() * 0.3f + m_pressSpring.GetValue() * 0.2f;

    rt->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale,
        D2D1::Point2F(m_x + m_width/2, m_y + m_height/2)));

    if (!m_roundedRect) {
        ID2D1Factory* factory = nullptr;
        rt->GetFactory(&factory);
        ID2D1RoundedRectangleGeometry* geo = nullptr;
        factory->CreateRoundedRectangleGeometry(
            D2D1::RoundedRect(D2D1::RectF(m_x, m_y, m_x+m_width, m_y+m_height), 8, 8),
            &geo);
        m_roundedRect.Attach(geo);
        factory->Release();
    }
    D2D1_COLOR_F bgCol = m_bgBrush->GetColor();
    bgCol.a = 0.05f + glowAlpha;
    m_bgBrush->SetColor(bgCol);
    rt->FillGeometry(m_roundedRect.Get(), m_bgBrush.Get());

    D2D1_COLOR_F borderCol = m_borderBrush->GetColor();
    borderCol.a = glowAlpha;
    m_borderBrush->SetColor(borderCol);
    rt->DrawGeometry(m_roundedRect.Get(), m_borderBrush.Get(), 1.5f);

    D2D1_COLOR_F textCol = m_style.textColor;
    textCol.a = m_enabled ? 1.0f : 0.4f;
    m_textBrush->SetColor(textCol);
    rt->DrawTextLayout(D2D1::Point2F(m_x, m_y), m_textLayout.Get(), m_textBrush.Get());

    rt->SetTransform(D2D1::Matrix3x2F::Identity());
}
