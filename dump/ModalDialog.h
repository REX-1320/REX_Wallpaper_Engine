#pragma once
#include <d2d1_3.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <string>
#include <functional>
#include "GlowStyle.h"
#include "LiquidGlassTokens.hpp"
#include "Button.h"
#include "SpringAnimation.h"

class ModalDialog {
public:
    ModalDialog(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, const GlowStyle& style);
    void SetTitle(const std::wstring& title);
    void SetContent(const std::wstring& content);
    void SetSize(float w, float h);
    void Show();
    void Close();
    bool IsVisible() const { return m_visible; }
    void HandleClick(float mx, float my);
    void Update(float dt);
    void Render(ID2D1RenderTarget* rt);

private:
    bool m_visible = false;
    float m_x, m_y, m_width = 400, m_height = 300;
    std::wstring m_title, m_content;
    SpringAnimation m_alpha{300,25,1,0};
    Button m_closeButton;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_titleFormat, m_contentFormat;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_overlay, m_bgBrush, m_textBrush;
    GlowStyle m_style;
    LiquidGlassTokens m_glassTokens = LiquidGlassTokens::DefaultDark();
};
