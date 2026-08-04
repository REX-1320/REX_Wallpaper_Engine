#include "Slider.h"
#include <sstream>
#include <algorithm>

Slider::Slider(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, const GlowStyle& style,
               float minVal, float maxVal, float step)
    : m_min(minVal), m_max(maxVal), m_step(step), m_style(style) {
    rt->CreateSolidColorBrush(D2D1::ColorF(0.5f,0.5f,0.5f,0.5f), &m_trackBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(1,1,1,1), &m_thumbBrush);
    rt->CreateSolidColorBrush(style.textColor, &m_textBrush);
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-us", &m_labelFormat);
}

void Slider::SetPosition(float x, float y, float width) {
    m_x = x; m_y = y; m_width = width;
}

void Slider::SetValue(float v) {
    m_value = std::clamp(v, m_min, m_max);
    m_thumbPos.SetTarget((m_value - m_min) / (m_max - m_min));
    if (m_onChange) m_onChange(m_value);
}

bool Slider::HitTest(float mx, float my) {
    return mx >= m_x && mx <= m_x + m_width && my >= m_y - 10 && my <= m_y + 20;
}

void Slider::Update(float dt) { m_thumbPos.Update(dt); }

void Slider::Render(ID2D1RenderTarget* rt) {
    float pos = m_thumbPos.GetValue();
    float thumbX = m_x + pos * m_width;
    D2D1::ColorF activeCol = m_style.glowColor; activeCol.a = 0.8f;
    m_trackBrush->SetColor(activeCol);
    rt->DrawLine(D2D1::Point2F(m_x, m_y+10), D2D1::Point2F(thumbX, m_y+10),
        m_trackBrush.Get(), 3);
    activeCol.a = 0.5f;
    m_trackBrush->SetColor(activeCol);
    rt->DrawLine(D2D1::Point2F(thumbX, m_y+10), D2D1::Point2F(m_x+m_width, m_y+10),
        m_trackBrush.Get(), 3);
    m_thumbBrush->SetColor(D2D1::ColorF(1,1,1,1));
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumbX, m_y+10), 8, 8), m_thumbBrush.Get());
    std::wstringstream ss; ss << (int)m_value;
    auto text = ss.str();
    rt->DrawTextW(text.c_str(), (UINT32)text.size(), m_labelFormat.Get(),
        D2D1::RectF(m_x+m_width+10, m_y, 60, 20), m_textBrush.Get());
}
