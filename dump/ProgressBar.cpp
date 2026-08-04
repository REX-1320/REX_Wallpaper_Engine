#include "ProgressBar.h"
#include <algorithm>

ProgressBar::ProgressBar(ID2D1RenderTarget* rt, const GlowStyle& style) : m_style(style) {
    if (rt) {
        rt->CreateSolidColorBrush(D2D1::ColorF(0.3f,0.3f,0.3f,0.5f), &m_trackBrush);
        rt->CreateSolidColorBrush(style.glowColor, &m_fillBrush);
    }
}

void ProgressBar::SetBounds(float x, float y, float width, float height) {
    m_x = x; m_y = y; m_width = width; m_height = height;
}

void ProgressBar::SetProgress(float p) {
    m_progress = std::clamp(p, 0.0f, 1.0f);
    m_progressSpring.SetTarget(m_progress);
    m_indeterminate = false;
}

void ProgressBar::SetIndeterminate(bool ind) { m_indeterminate = ind; }

void ProgressBar::Update(float dt) {
    m_progressSpring.Update(dt);
    if (m_indeterminate) {
        m_indeterminatePos += dt * 0.5f;
        if (m_indeterminatePos > 1.5f) m_indeterminatePos = -0.5f;
    }
}

void ProgressBar::Render(ID2D1RenderTarget* rt) {
    D2D1_RECT_F track = {m_x, m_y, m_x + m_width, m_y + m_height};
    rt->FillRoundedRectangle(D2D1::RoundedRect(track, m_height/2, m_height/2), m_trackBrush.Get());
    float fillWidth;
    if (m_indeterminate) {
        fillWidth = m_width * 0.3f;
        float start = (m_indeterminatePos * m_width) - fillWidth/2;
        start = std::clamp(start, 0.0f, m_width - fillWidth);
        D2D1_RECT_F fill = {m_x + start, m_y, m_x + start + fillWidth, m_y + m_height};
        rt->FillRoundedRectangle(D2D1::RoundedRect(fill, m_height/2, m_height/2), m_fillBrush.Get());
    } else {
        float p = m_progressSpring.GetValue();
        D2D1_RECT_F fill = {m_x, m_y, m_x + p * m_width, m_y + m_height};
        rt->FillRoundedRectangle(D2D1::RoundedRect(fill, m_height/2, m_height/2), m_fillBrush.Get());
    }
}
