#pragma once
#include "SpringAnimation.h"
#include "GlowStyle.h"
#include <d2d1_3.h>
#include <dwrite_3.h>
#include <wrl/client.h>
#include <string>

using Microsoft::WRL::ComPtr;

class GlowTextInput {
public:
    GlowTextInput(float x, float y, float width, ID2D1RenderTarget* rt,
                  IDWriteFactory* dwriteFactory, const GlowStyle& style);
    void SetText(const std::wstring& text) { m_text = text; RecreateLayout(); }
    std::wstring GetText() const { return m_text; }
    void SetFocused(bool focused);
    void SetPosition(float x, float y) { m_x = x; m_y = y; }
    void SetWidth(float w) { m_width = w; }
    bool HitTest(float x, float y);
    bool GetFocused() const { return m_focused; }
    void Update(float deltaTime);
    void Render(ID2D1RenderTarget* rt);

private:
    float m_x, m_y, m_width;
    std::wstring m_text;
    bool m_focused = false;
    SpringAnimation m_focusSpring;
    GlowStyle m_style;
    ComPtr<IDWriteFactory> m_dwriteFactory;
    ComPtr<IDWriteTextFormat> m_format;
    ComPtr<IDWriteTextLayout> m_layout;
    ComPtr<ID2D1SolidColorBrush> m_textBrush;
    ComPtr<ID2D1SolidColorBrush> m_underlineBrush;
    void RecreateLayout();
};
