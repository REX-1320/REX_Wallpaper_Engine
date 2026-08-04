#pragma once
#include <d2d1_3.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <string>
#include "GlowStyle.h"

class Tooltip {
public:
    Tooltip(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, const GlowStyle& style);
    void ShowAt(const std::wstring& text, float mx, float my);
    void Hide();
    void Update(float dt);
    void Render(ID2D1RenderTarget* rt);

private:
    std::wstring m_text;
    float m_x, m_y;
    bool m_visible = false;
    float m_timer = 0;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_format;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_bgBrush, m_textBrush;
    GlowStyle m_style;
};
