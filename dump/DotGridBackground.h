#pragma once
#include <d2d1_3.h>
#include <vector>
#include <windows.h>
#include <wrl/client.h>
#include <cmath>
#include <algorithm>

struct Dot {
    float cx, cy;
    float ox, oy;
    float vx, vy;
    bool inertiaActive;
};

class DotGridBackground {
public:
    DotGridBackground(int dotSize = 5, int gap = 25);
    void Build(int width, int height);
    void Update(float dt, float mouseX, float mouseY, bool mouseDown);
    void Render(ID2D1RenderTarget* rt);
    void SetColors(D2D1::ColorF base, D2D1::ColorF active);

private:
    int m_dotSize, m_gap;
    std::vector<Dot> m_dots;
    D2D1::ColorF m_baseColor, m_activeColor;
    float m_proximity = 150.0f;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_brush;
};
