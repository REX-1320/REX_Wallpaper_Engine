#pragma once
#include <vector>
#include <string>
#include <functional>
#include <d2d1_3.h>
#include <dwrite.h>
#include <wrl/client.h>
#include "GlowStyle.h"
#include "SpringAnimation.h"

class Tabs {
public:
    Tabs(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, const GlowStyle& style);
    void SetBounds(float x, float y, float width);
    void SetTabs(const std::vector<std::wstring>& labels);
    void SetSelected(int index);
    int GetSelected() const { return m_selectedIndex; }
    void SetOnChange(std::function<void(int)> cb) { m_onChange = std::move(cb); }

    bool HitTest(float mx, float my);
    void Update(float dt);
    void Render(ID2D1RenderTarget* rt);

private:
    float m_x, m_y, m_width;
    float m_tabHeight = 36;
    std::vector<std::wstring> m_labels;
    int m_selectedIndex = 0;
    int m_hoveredIndex = -1;

    std::vector<float> m_tabPositions;
    std::vector<float> m_tabWidths;

    SpringAnimation m_indicatorX{300, 25, 1, 0};
    SpringAnimation m_indicatorWidth{300, 25, 1, 0};

    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_textFormat;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_textBrush, m_activeBrush, m_indicatorBrush, m_hoverBrush;
    GlowStyle m_style;
    std::function<void(int)> m_onChange;
};
