#pragma once
#include "SpringAnimation.h"
#include "GlowStyle.h"
#include <d2d1_3.h>
#include <wrl/client.h>

class ProgressBar {
public:
    ProgressBar(ID2D1RenderTarget* rt, const GlowStyle& style);
    void SetBounds(float x, float y, float width, float height = 6);
    void SetProgress(float p);
    void SetIndeterminate(bool ind);
    void Update(float dt);
    void Render(ID2D1RenderTarget* rt);

private:
    float m_x, m_y, m_width, m_height;
    float m_progress = 0;
    bool m_indeterminate = false;
    float m_indeterminatePos = 0;
    SpringAnimation m_progressSpring{200,20,1,0};
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_trackBrush, m_fillBrush;
    GlowStyle m_style;
};
