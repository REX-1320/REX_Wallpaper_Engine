#pragma once
#include <d2d1_3.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <string>
#include "GlowStyle.h"

class Badge {
public:
    Badge(const std::wstring& text, ID2D1RenderTarget* rt, IDWriteFactory* dwrite, const GlowStyle& style);
    void SetPosition(float x, float y);
    void Render(ID2D1RenderTarget* rt);

private:
    std::wstring m_text;
    float m_x, m_y;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_format;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_bgBrush, m_textBrush;
    GlowStyle m_style;
};
