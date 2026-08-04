#pragma once
#include <vector>
#include <string>
#include <functional>
#include <d2d1_3.h>
#include <dwrite.h>
#include <wrl/client.h>
#include "GlowStyle.h"

struct BreadcrumbItem {
    std::wstring label;
    std::function<void()> onClick;
};

class Breadcrumb {
public:
    Breadcrumb(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, const GlowStyle& style);
    void SetBounds(float x, float y);
    void SetPath(const std::vector<BreadcrumbItem>& items);
    bool HitTest(float mx, float my);
    void Render(ID2D1RenderTarget* rt);

private:
    float m_x, m_y;
    std::vector<BreadcrumbItem> m_items;
    std::vector<float> m_itemWidths;
    int m_hoveredIndex = -1;

    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_textFormat;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_textBrush, m_activeBrush, m_separatorBrush;
    GlowStyle m_style;
};
