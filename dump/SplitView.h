#pragma once
#include <functional>
#include <algorithm>
#include <d2d1_3.h>
#include <wrl/client.h>
#include "GlowStyle.h"

class SplitView {
public:
    enum class Orientation { Horizontal, Vertical };

    SplitView(ID2D1RenderTarget* rt, const GlowStyle& style, Orientation orient = Orientation::Horizontal);
    void SetBounds(float x, float y, float width, float height);
    void SetSplitPosition(float pos);
    float GetSplitPosition() const { return m_splitPos; }

    void GetFirstPaneRect(float& x, float& y, float& w, float& h) const;
    void GetSecondPaneRect(float& x, float& y, float& w, float& h) const;

    bool HitTestDivider(float mx, float my);
    void StartResize();
    void ResizeTo(float mx, float my);
    void EndResize();

    void Render(ID2D1RenderTarget* rt);

private:
    float m_x, m_y, m_width, m_height;
    float m_splitPos = 0.25f;
    float m_minPaneSize = 100;
    Orientation m_orient;
    bool m_resizing = false;

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_dividerBrush;
    GlowStyle m_style;
};
