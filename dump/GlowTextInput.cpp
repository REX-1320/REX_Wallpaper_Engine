#include "GlowTextInput.h"

GlowTextInput::GlowTextInput(float x, float y, float width, ID2D1RenderTarget* rt,
                             IDWriteFactory* dwriteFactory, const GlowStyle& style)
    : m_x(x), m_y(y), m_width(width), m_style(style),
      m_focusSpring(200.0f, 20.0f, 1.0f, 0.0f),
      m_dwriteFactory(dwriteFactory)
{
    if (dwriteFactory) {
        dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            18.0f, L"en-us", &m_format);
        if (m_format) m_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        RecreateLayout();
    }
    if (rt) {
        rt->CreateSolidColorBrush(style.textColor, &m_textBrush);
        rt->CreateSolidColorBrush(style.glowColor, &m_underlineBrush);
    }
}

void GlowTextInput::SetFocused(bool focused) {
    m_focused = focused;
    m_focusSpring.SetTarget(focused ? 1.0f : 0.0f);
}

bool GlowTextInput::HitTest(float x, float y) {
    if (!m_layout) return false;
    DWRITE_TEXT_METRICS metrics;
    m_layout->GetMetrics(&metrics);
    float right = m_x + m_width;
    float bottom = m_y + metrics.height;
    return x >= m_x && x <= right && y >= m_y && y <= bottom;
}

void GlowTextInput::Update(float deltaTime) {
    m_focusSpring.Update(deltaTime);
}

void GlowTextInput::Render(ID2D1RenderTarget* rt) {
    if (!rt) return;
    if (!m_underlineBrush || !m_textBrush || !m_layout) return;
    float underlineAlpha = m_focusSpring.GetValue() * 0.8f + 0.2f;
    D2D1::ColorF col = m_style.glowColor;
    col.a = underlineAlpha;
    m_underlineBrush->SetColor(col);

    rt->DrawTextLayout(D2D1::Point2F(m_x, m_y), m_layout.Get(), m_textBrush.Get());

    D2D1_RECT_F underline = D2D1::RectF(m_x, m_y + 22, m_x + m_width, m_y + 24);
    rt->FillRectangle(underline, m_underlineBrush.Get());
}

void GlowTextInput::RecreateLayout() {
    if (!m_dwriteFactory) return;
    ComPtr<IDWriteTextLayout> newLayout;
    if (m_text.empty()) {
        m_dwriteFactory->CreateTextLayout(L" ", 1, m_format.Get(), m_width, 100.0f, &newLayout);
    } else {
        m_dwriteFactory->CreateTextLayout(m_text.c_str(), (UINT32)m_text.size(),
            m_format.Get(), m_width, 100.0f, &newLayout);
    }
    m_layout = std::move(newLayout);
}
