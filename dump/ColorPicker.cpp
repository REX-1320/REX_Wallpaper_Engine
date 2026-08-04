#include "ColorPicker.h"

ColorPicker::ColorPicker(ID2D1RenderTarget* rt, const GlowStyle& style) : m_style(style) {
    float vals[] = {1,0.8f,0.6f,0.4f,0.2f,0};
    for (float r : vals)
        for (float g : vals)
            for (float b : vals)
                m_colors.push_back(D2D1::ColorF(r,g,b,1));
}

void ColorPicker::SetPosition(float x, float y) { m_x = x; m_y = y; }

bool ColorPicker::HitTest(float mx, float my) {
    return mx >= m_x && mx <= m_x + m_columns*(m_size+m_gap) &&
           my >= m_y && my <= m_y + ((int)m_colors.size()/m_columns)*(m_size+m_gap);
}

void ColorPicker::Render(ID2D1RenderTarget* rt) {
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    rt->CreateSolidColorBrush(D2D1::ColorF(1,1,1,1), &brush);
    for (size_t i = 0; i < m_colors.size(); ++i) {
        int col = (int)(i % m_columns);
        int row = (int)(i / m_columns);
        float cx = m_x + col * (m_size + m_gap);
        float cy = m_y + row * (m_size + m_gap);
        brush->SetColor(m_colors[i]);
        rt->FillRoundedRectangle(D2D1::RoundedRect(
            D2D1_RECT_F{cx, cy, cx+m_size, cy+m_size}, 4, 4), brush.Get());
    }
}
