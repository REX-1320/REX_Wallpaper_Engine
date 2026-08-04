#pragma once
#include "SpringAnimation.h"
#include "GlowStyle.h"
#include <d2d1_3.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <string>
#include <functional>

class Slider {
public:
    Slider(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, const GlowStyle& style,
           float minVal = 0, float maxVal = 100, float step = 1);
    void SetPosition(float x, float y, float width);
    void SetValue(float v);
    float GetValue() const { return m_value; }
    void SetOnChange(std::function<void(float)> cb) { m_onChange = std::move(cb); }
    bool HitTest(float mx, float my);
    void Update(float dt);
    void Render(ID2D1RenderTarget* rt);

private:
    float m_x = 0, m_y = 0, m_width = 200;
    float m_min, m_max, m_step;
    float m_value = 0;
    SpringAnimation m_thumbPos{300,25,1,0};
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_trackBrush, m_thumbBrush, m_textBrush;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_labelFormat;
    GlowStyle m_style;
    std::function<void(float)> m_onChange;
};
