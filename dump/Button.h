#pragma once
#include "SpringAnimation.h"
#include "GlowStyle.h"
#include <d2d1_3.h>
#include <dwrite_3.h>
#include <wrl/client.h>
#include <string>
#include <functional>

class Button {
public:
    Button(const std::wstring& text, ID2D1RenderTarget* rt,
           IDWriteFactory* dwrite, const GlowStyle& style,
           const std::wstring& icon = L"");

    void SetPosition(float x, float y) { m_x = x; m_y = y; }
    void SetSize(float width, float height) { m_width = width; m_height = height; }
    void SetOnClick(std::function<void()> cb) { m_onClick = std::move(cb); }
    std::function<void()> GetOnClick() const { return m_onClick; }

    bool HitTest(float mx, float my);
    void Update(float dt);
    void Render(ID2D1RenderTarget* rt);

    void SetHovered(bool h) { m_hoverSpring.SetTarget(h ? 1.0f : 0.0f); }
    void SetPressed(bool p) { m_pressSpring.SetTarget(p ? 1.0f : 0.0f); }
    void SetEnabled(bool e) { m_enabled = e; }

private:
    std::wstring m_text, m_icon;
    float m_x = 0, m_y = 0, m_width = 120, m_height = 36;
    bool m_enabled = true;

    SpringAnimation m_hoverSpring{300,25,1,0};
    SpringAnimation m_pressSpring{500,30,1,0};
    SpringAnimation m_scaleSpring{400,30,1,1};

    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_textFormat;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> m_textLayout;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_textBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_bgBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_borderBrush;
    Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> m_roundedRect;

    GlowStyle m_style;
    std::function<void()> m_onClick;
};
