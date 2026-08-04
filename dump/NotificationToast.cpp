#include "NotificationToast.h"

NotificationToast::NotificationToast(const std::wstring& title, const std::wstring& message,
                                     ID2D1RenderTarget* rt, IDWriteFactory* dwriteFactory,
                                     const GlowStyle& style)
    : m_title(title), m_message(message), m_style(style),
      m_opacitySpring(100.0f, 15.0f, 1.0f, 1.0f)
{
    if (dwriteFactory) {
        dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16.0f, L"en-us", &m_titleFormat);
        dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"en-us", &m_msgFormat);
        if (m_titleFormat)
            dwriteFactory->CreateTextLayout(title.c_str(), (UINT32)title.size(), m_titleFormat.Get(), 250.0f, 30.0f, &m_titleLayout);
        if (m_msgFormat)
            dwriteFactory->CreateTextLayout(message.c_str(), (UINT32)message.size(), m_msgFormat.Get(), 250.0f, 40.0f, &m_msgLayout);
    }
    if (rt) {
        rt->CreateSolidColorBrush(style.textColor, &m_titleBrush);
        rt->CreateSolidColorBrush(style.textColor, &m_msgBrush);
    }
}

void NotificationToast::Update(float deltaTime) {
    m_opacitySpring.Update(deltaTime);
    m_opacity = m_opacitySpring.GetValue();
    m_lifetime -= deltaTime;
    if (m_lifetime <= 1.0f && m_opacitySpring.GetTarget() > 0.0f) {
        Dismiss();
    }
}

void NotificationToast::Render(ID2D1RenderTarget* rt) {
    if (m_opacity <= 0.01f) return;
    D2D1::ColorF col = m_style.textColor;
    col.a = m_opacity;
    m_titleBrush->SetColor(col);
    m_msgBrush->SetColor(col);
    rt->DrawTextLayout(D2D1::Point2F(m_x, m_y), m_titleLayout.Get(), m_titleBrush.Get());
    rt->DrawTextLayout(D2D1::Point2F(m_x, m_y + 20), m_msgLayout.Get(), m_msgBrush.Get());
}

void NotificationToast::Dismiss() {
    m_opacitySpring.SetTarget(0.0f);
}
