#include "Toggle.h"

Toggle::Toggle(ID2D1RenderTarget* rt, const GlowStyle& style) : m_style(style) {
    rt->CreateSolidColorBrush(D2D1::ColorF(0.5f,0.5f,0.5f,0.5f), &m_trackBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(1,1,1,1), &m_thumbBrush);
}

void Toggle::SetChecked(bool checked) {
    m_checked = checked;
    m_thumbSpring.SetTarget(checked ? 1.0f : 0.0f);
    if (m_onChange) m_onChange(m_checked);
}

bool Toggle::HitTest(float mx, float my) {
    return mx >= m_x - 4 && mx <= m_x + m_width + 4 && my >= m_y - 4 && my <= m_y + m_height + 4;
}

void Toggle::Update(float dt) { m_thumbSpring.Update(dt); }

void Toggle::Render(ID2D1RenderTarget* rt) {
    D2D1_RECT_F track = D2D1::RectF(m_x, m_y, m_x + m_width, m_y + m_height);
    float radius = m_height / 2;
    rt->FillRoundedRectangle(D2D1::RoundedRect(track, radius, radius), m_trackBrush.Get());
    float pos = m_thumbSpring.GetValue();
    float thumbX = m_x + 2 + pos * (m_width - m_height - 4);
    if (pos > 0.5f) {
        m_trackBrush->SetColor(D2D1::ColorF(m_style.glowColor.r, m_style.glowColor.g,
            m_style.glowColor.b, 0.6f));
    }
    D2D1_ELLIPSE thumb = {D2D1::Point2F(thumbX + m_height/2, m_y + m_height/2),
        m_height/2 - 2, m_height/2 - 2};
    rt->FillEllipse(&thumb, m_thumbBrush.Get());
}
