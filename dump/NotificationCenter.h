#pragma once
#include <d2d1_3.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <vector>
#include <string>
#include <memory>
#include "GlowStyle.h"
#include "SpringAnimation.h"
#include "Button.h"
#include "SectionHeader.h"

namespace rex::notifications {
    class INotificationService;
    struct Notification;
}

class NotificationCenter {
public:
    NotificationCenter(ID2D1RenderTarget* rt, IDWriteFactory* dwrite,
                       const GlowStyle& style,
                       std::shared_ptr<rex::notifications::INotificationService> service);

    void SetBounds(float x, float y, float w, float h);
    void Show();
    void Hide();
    void Toggle();
    bool IsVisible() const { return m_visible; }

    void Update(float dt);
    void Render(ID2D1RenderTarget* rt);
    bool HitTest(float mx, float my);
    void HandleClick(float mx, float my);

private:
    std::shared_ptr<rex::notifications::INotificationService> m_service;

    float m_x=0, m_y=0, m_w=340, m_h=480;
    bool m_visible = false;
    SpringAnimation m_slideSpring{300, 25, 1, 0};
    SpringAnimation m_fadeSpring{200, 20, 1, 0};

    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_titleFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_bodyFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_timeFormat;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_bgBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_borderBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_textBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_secondaryBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_dividerBrush;
    Button m_clearAllBtn;
};
