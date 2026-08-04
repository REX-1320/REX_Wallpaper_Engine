#pragma once
#include <d2d1_3.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <string>
#include <functional>
#include "GlowStyle.h"
#include "Button.h"

class AlertDialog {
public:
    AlertDialog(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, const GlowStyle& style);
    void Show(const std::wstring& message, std::function<void()> onOk = nullptr);
    bool IsVisible() const { return m_visible; }
    void HandleClick(float mx, float my);
    void Render(ID2D1RenderTarget* rt);

private:
    bool m_visible = false;
    float m_x, m_y, m_width = 360, m_height = 160;
    std::wstring m_message;
    Button m_okButton;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_format;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_bgBrush, m_textBrush;
    GlowStyle m_style;
    std::function<void()> m_onOk;
};
