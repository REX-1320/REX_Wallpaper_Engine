#pragma once
#include <d2d1_3.h>
#include <wrl/client.h>
#include <cmath>

class Skeleton {
public:
    Skeleton(ID2D1RenderTarget* rt);
    void SetBounds(float x, float y, float width, float height);
    void Update(float dt);
    void Render(ID2D1RenderTarget* rt);

private:
    float m_x, m_y, m_width, m_height;
    float m_animPhase = 0;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brush;
};
