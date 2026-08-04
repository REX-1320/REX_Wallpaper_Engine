#include "SplitView.h"

SplitView::SplitView(ID2D1RenderTarget* rt, const GlowStyle& style, Orientation orient)
    : m_style(style), m_orient(orient) {
    if (rt) {
        rt->CreateSolidColorBrush(D2D1::ColorF(0.3f,0.3f,0.3f,0.5f), &m_dividerBrush);
    }
}

void SplitView::SetBounds(float x, float y, float width, float height) {
    m_x = x; m_y = y; m_width = width; m_height = height;
}

void SplitView::SetSplitPosition(float pos) {
    m_splitPos = std::clamp(pos, m_minPaneSize / m_width, 1.0f - m_minPaneSize / m_width);
}

void SplitView::GetFirstPaneRect(float& x, float& y, float& w, float& h) const {
    x = m_x; y = m_y;
    if (m_orient == Orientation::Horizontal) {
        w = m_width * m_splitPos; h = m_height;
    } else {
        w = m_width; h = m_height * m_splitPos;
    }
}

void SplitView::GetSecondPaneRect(float& x, float& y, float& w, float& h) const {
    if (m_orient == Orientation::Horizontal) {
        float pw = m_width * m_splitPos;
        x = m_x + pw + 4; y = m_y; w = m_width - pw - 4; h = m_height;
    } else {
        float ph = m_height * m_splitPos;
        x = m_x; y = m_y + ph + 4; w = m_width; h = m_height - ph - 4;
    }
}

bool SplitView::HitTestDivider(float mx, float my) {
    float divX, divY, divW, divH;
    if (m_orient == Orientation::Horizontal) {
        divX = m_x + m_width * m_splitPos - 2; divY = m_y;
        divW = 4; divH = m_height;
    } else {
        divX = m_x; divY = m_y + m_height * m_splitPos - 2;
        divW = m_width; divH = 4;
    }
    return mx >= divX && mx <= divX + divW && my >= divY && my <= divY + divH;
}

void SplitView::StartResize() { m_resizing = true; }

void SplitView::ResizeTo(float mx, float my) {
    if (!m_resizing) return;
    if (m_orient == Orientation::Horizontal) {
        SetSplitPosition((mx - m_x) / m_width);
    } else {
        SetSplitPosition((my - m_y) / m_height);
    }
}

void SplitView::EndResize() { m_resizing = false; }

void SplitView::Render(ID2D1RenderTarget* rt) {
    float divX, divY, divW, divH;
    if (m_orient == Orientation::Horizontal) {
        divX = m_x + m_width * m_splitPos - 1; divY = m_y;
        divW = 2; divH = m_height;
    } else {
        divX = m_x; divY = m_y + m_height * m_splitPos - 1;
        divW = m_width; divH = 2;
    }
    D2D1_RECT_F divRect = {divX, divY, divX+divW, divY+divH};
    rt->FillRectangle(divRect, m_dividerBrush.Get());
}
