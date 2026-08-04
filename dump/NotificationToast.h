#pragma once
#include "SpringAnimation.h"
#include "GlowStyle.h"
#include <d2d1_3.h>
#include <dwrite_3.h>
#include <wrl/client.h>
#include <string>
#include <functional>

using Microsoft::WRL::ComPtr;

class NotificationToast {
public:
    NotificationToast(const std::wstring& title, const std::wstring& message,
                      ID2D1RenderTarget* rt, IDWriteFactory* dwriteFactory,
                      const GlowStyle& style);
    void Update(float deltaTime);
    void Render(ID2D1RenderTarget* rt);
    bool IsExpired() const { return m_opacity <= 0.01f; }
    void Dismiss();
    void SetPosition(float x, float y) { m_x = x; m_y = y; }
    void SetOnDismiss(std::function<void()> cb) { m_onDismiss = std::move(cb); }
private:
    std::wstring m_title, m_message;
    std::function<void()> m_onDismiss;
    float m_x, m_y;
    float m_opacity = 1.0f;
    float m_lifetime = 5.0f;
    SpringAnimation m_opacitySpring;
    GlowStyle m_style;
    ComPtr<IDWriteTextFormat> m_titleFormat, m_msgFormat;
    ComPtr<IDWriteTextLayout> m_titleLayout, m_msgLayout;
    ComPtr<ID2D1SolidColorBrush> m_titleBrush, m_msgBrush;
};
