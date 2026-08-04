#include "Skeleton.h"

Skeleton::Skeleton(ID2D1RenderTarget* rt) {
    rt->CreateSolidColorBrush(D2D1::ColorF(1,1,1,0.1f), &m_brush);
}

void Skeleton::SetBounds(float x, float y, float width, float height) {
    m_x = x; m_y = y; m_width = width; m_height = height;
}

void Skeleton::Update(float dt) {
    m_animPhase += dt * 2.0f;
    if (m_animPhase > 1.0f) m_animPhase -= 1.0f;
}

void Skeleton::Render(ID2D1RenderTarget* rt) {
    float alpha = 0.05f + 0.05f * sinf(m_animPhase * 3.14159f * 2);
    m_brush->SetColor(D2D1::ColorF(1,1,1,alpha));
    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(m_x, m_y, m_x+m_width, m_y+m_height), 8, 8), m_brush.Get());
}
