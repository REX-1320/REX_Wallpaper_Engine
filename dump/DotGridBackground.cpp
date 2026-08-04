#include "DotGridBackground.h"

DotGridBackground::DotGridBackground(int dotSize, int gap)
    : m_dotSize(dotSize), m_gap(gap),
      m_baseColor(D2D1::ColorF(0.9f, 0.9f, 0.9f, 0.3f)),
      m_activeColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.8f)) {}

void DotGridBackground::Build(int width, int height) {
    m_dots.clear();
    float cell = (float)(m_dotSize + m_gap);
    int cols = (int)((width + m_gap) / cell);
    int rows = (int)((height + m_gap) / cell);
    float startX = ((float)width - (cols * cell - m_gap)) / 2.0f + m_dotSize / 2.0f;
    float startY = ((float)height - (rows * cell - m_gap)) / 2.0f + m_dotSize / 2.0f;
    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < cols; ++x) {
            Dot d;
            d.cx = startX + x * cell;
            d.cy = startY + y * cell;
            d.ox = d.oy = d.vx = d.vy = 0;
            d.inertiaActive = false;
            m_dots.push_back(d);
        }
    }
}

void DotGridBackground::Update(float dt, float mouseX, float mouseY, bool mouseDown) {
    float proxSq = m_proximity * m_proximity;
    float damp = 0.85f;
    for (auto& dot : m_dots) {
        float dx = dot.cx - mouseX;
        float dy = dot.cy - mouseY;
        float distSq = dx * dx + dy * dy;
        if (distSq < proxSq && distSq > 0.0f) {
            float dist = sqrtf(distSq);
            float force = (m_proximity - dist) / m_proximity * 400.0f;
            dot.vx += (dx / dist) * force * dt;
            dot.vy += (dy / dist) * force * dt;
        }
        float dampingFactor = powf(damp, dt * 60.0f);
        dot.vx *= dampingFactor;
        dot.vy *= dampingFactor;
        float springStiff = 10.0f;
        dot.vx -= dot.ox * springStiff * dt;
        dot.vy -= dot.oy * springStiff * dt;
        dot.ox += dot.vx * dt;
        dot.oy += dot.vy * dt;
    }
}

void DotGridBackground::Render(ID2D1RenderTarget* rt) {
    if (!m_brush) {
        rt->CreateSolidColorBrush(m_baseColor, &m_brush);
    }
    float radius = m_dotSize / 2.0f;
    for (auto& dot : m_dots) {
        float x = dot.cx + dot.ox;
        float y = dot.cy + dot.oy;
        float dist = sqrtf(dot.ox * dot.ox + dot.oy * dot.oy);
        float t = std::min(dist / 50.0f, 1.0f);
        D2D1::ColorF col(
            m_baseColor.r + (m_activeColor.r - m_baseColor.r) * t,
            m_baseColor.g + (m_activeColor.g - m_baseColor.g) * t,
            m_baseColor.b + (m_activeColor.b - m_baseColor.b) * t,
            m_baseColor.a + (m_activeColor.a - m_baseColor.a) * t);
        m_brush->SetColor(col);
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), radius, radius), m_brush.Get());
    }
}

void DotGridBackground::SetColors(D2D1::ColorF base, D2D1::ColorF active) {
    m_baseColor = base;
    m_activeColor = active;
}
