#pragma once
#include <d2d1_3.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <string>
#include <functional>
#include <vector>
#include "Button.h"
#include "GlowStyle.h"
#include "LiquidGlassTokens.hpp"
#include "SpringAnimation.h"

class Card {
public:
    Card(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, const GlowStyle& style);

    void SetBounds(float x, float y, float w, float h);
    void SetTitle(const std::wstring& title);
    void SetSubtitle(const std::wstring& subtitle);
    void SetImagePlaceholder(bool show) { m_showImage = show; }
    void AddAction(const std::wstring& label, std::function<void()> cb);

    bool HitTest(float mx, float my);
    void Update(float dt);
    void Render(ID2D1RenderTarget* rt);

    void SetHovered(bool h) { m_hovered = h; m_elevationSpring.SetTarget(h ? 1.0f : 0.0f); }

private:
    float m_x=0, m_y=0, m_width=280, m_height=200;
    std::wstring m_title, m_subtitle;
    bool m_showImage = false;
    bool m_hovered = false;
    SpringAnimation m_elevationSpring{300, 25, 1, 0};
    LiquidGlassTokens m_glassTokens = LiquidGlassTokens::DefaultDark();

    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_titleFormat, m_subtitleFormat;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> m_titleLayout, m_subtitleLayout;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_bgBrush, m_borderBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_textBrush, m_subtitleBrush;
    Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> m_roundedRect;

    std::vector<Button> m_actionButtons;
    GlowStyle m_style;
    IDWriteFactory* m_dwrite = nullptr;
    ID2D1RenderTarget* m_rt = nullptr;
};
