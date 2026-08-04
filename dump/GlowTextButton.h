#pragma once
#include "SpringAnimation.h"
#include "GlowStyle.h"
#include <d2d1_3.h>
#include <dwrite_3.h>
#include <wrl/client.h>
#include <string>
#include <functional>

using Microsoft::WRL::ComPtr;

class GlowTextButton {
public:
    GlowTextButton(const std::wstring& text, ID2D1RenderTarget* rt,
                   IDWriteFactory* dwriteFactory, const GlowStyle& style);
    ~GlowTextButton() = default;

    void SetPosition(float x, float y) { m_x = x; m_y = y; }
    void SetFontSize(float size);
    void SetSelected(bool selected);
    void SetHovered(bool hovered);
    void SetOnClick(std::function<void()> callback) { m_onClick = std::move(callback); }
    std::function<void()> GetOnClick() const { return m_onClick; }
    bool HitTest(float x, float y);

    void Update(float deltaTime);
    void Render(ID2D1RenderTarget* rt);

private:
    std::wstring m_text;
    float m_x, m_y, m_fontSize = 16.0f;
    bool m_selected = false, m_hovered = false;
    SpringAnimation m_selectSpring;
    SpringAnimation m_scaleSpring;
    SpringAnimation m_hoverSpring;

    ComPtr<IDWriteFactory> m_dwriteFactory;
    ComPtr<IDWriteTextFormat> m_textFormat;
    ComPtr<IDWriteTextLayout> m_textLayout;
    ComPtr<ID2D1SolidColorBrush> m_textBrush;
    ComPtr<ID2D1SolidColorBrush> m_glowBrush;

    GlowStyle m_style;
    std::function<void()> m_onClick;

    void CreateTextLayout(IDWriteFactory* dwriteFactory);
};
