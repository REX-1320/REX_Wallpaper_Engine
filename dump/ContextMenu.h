#pragma once
#include <d2d1_3.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <vector>
#include <string>
#include <functional>
#include "GlowStyle.h"

struct ContextMenuItem {
    std::wstring label;
    std::function<void()> action;
    bool enabled = true;
    bool separator = false;
};

class ContextMenu {
public:
    ContextMenu(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, const GlowStyle& style);
    void SetItems(const std::vector<ContextMenuItem>& items);
    void ShowAt(float mx, float my);
    void Hide();
    bool IsVisible() const { return m_visible; }
    void HandleClick(float mx, float my);
    void Update(float dt);
    void Render(ID2D1RenderTarget* rt);

private:
    bool m_visible = false;
    float m_x, m_y, m_width = 180;
    std::vector<ContextMenuItem> m_items;
    int m_hoveredIndex = -1;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_format;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_bgBrush, m_textBrush, m_hoverBrush, m_sepBrush;
    GlowStyle m_style;
};
