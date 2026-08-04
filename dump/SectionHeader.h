#pragma once
#include <d2d1_3.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <string>
#include <functional>
#include "GlowStyle.h"

class SectionHeader {
public:
    SectionHeader(const std::wstring& title, ID2D1RenderTarget* rt, IDWriteFactory* dwrite,
                  const GlowStyle& style, const std::wstring& actionLabel = L"",
                  std::function<void()> actionCb = nullptr);

    void SetPosition(float x, float y) { m_x = x; m_y = y; }
    void Render(ID2D1RenderTarget* rt);
    bool HitTestAction(float mx, float my);

private:
    float m_x = 0, m_y = 0;
    std::wstring m_title, m_actionLabel;
    std::function<void()> m_actionCb;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_titleFormat, m_actionFormat;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> m_titleLayout, m_actionLayout;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_titleBrush, m_actionBrush;
    GlowStyle m_style;
};
