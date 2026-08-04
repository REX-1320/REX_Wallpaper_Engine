#pragma once
#include "SpringAnimation.h"
#include "GlowStyle.h"
#include <d2d1_3.h>
#include <wrl/client.h>
#include <functional>

class Toggle {
public:
    Toggle(ID2D1RenderTarget* rt, const GlowStyle& style);
    void SetPosition(float x, float y) { m_x = x; m_y = y; }
    void SetChecked(bool checked);
    bool IsChecked() const { return m_checked; }
    void SetOnChange(std::function<void(bool)> cb) { m_onChange = std::move(cb); }
    bool HitTest(float mx, float my);
    void Update(float dt);
    void Render(ID2D1RenderTarget* rt);

private:
    float m_x = 0, m_y = 0;
    float m_width = 44, m_height = 24;
    bool m_checked = false;
    SpringAnimation m_thumbSpring{400,30,1,0};
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_trackBrush, m_thumbBrush;
    GlowStyle m_style;
    std::function<void(bool)> m_onChange;
};
