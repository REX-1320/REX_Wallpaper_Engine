#pragma once
#include <d2d1_3.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <vector>
#include <string>
#include <functional>
#include "GlowStyle.h"

class Dropdown {
public:
    Dropdown(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, const GlowStyle& style);
    void SetBounds(float x, float y, float width);
    void SetItems(const std::vector<std::wstring>& items);
    void SetSelected(int index);
    int GetSelected() const { return m_selectedIndex; }
    void SetOnChange(std::function<void(int)> cb) { m_onChange = std::move(cb); }
    bool HitTest(float mx, float my);
    void HandleClick(float mx, float my);
    void Render(ID2D1RenderTarget* rt);

private:
    float m_x, m_y, m_width, m_height = 32;
    std::vector<std::wstring> m_items;
    int m_selectedIndex = 0;
    bool m_open = false;
    std::function<void(int)> m_onChange;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_format;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_bgBrush, m_textBrush, m_itemBrush;
    GlowStyle m_style;
};
