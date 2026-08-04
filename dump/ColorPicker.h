#pragma once
#include <d2d1_3.h>
#include <wrl/client.h>
#include <vector>
#include <functional>
#include "GlowStyle.h"

class ColorPicker {
public:
    ColorPicker(ID2D1RenderTarget* rt, const GlowStyle& style);
    void SetPosition(float x, float y);
    void SetOnColor(std::function<void(D2D1_COLOR_F)> cb) { m_onColor = std::move(cb); }
    bool HitTest(float mx, float my);
    void Render(ID2D1RenderTarget* rt);

private:
    float m_x, m_y;
    float m_size = 24, m_gap = 4;
    int m_columns = 8;
    std::vector<D2D1_COLOR_F> m_colors;
    std::function<void(D2D1_COLOR_F)> m_onColor;
    GlowStyle m_style;
};
