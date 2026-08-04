#include "ModalDialog.h"
#include "graphics/effects/LiquidGlassBrush.hpp"

ModalDialog::ModalDialog(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, const GlowStyle& style)
    : m_closeButton(L"\u00D7", rt, dwrite, style), m_style(style) {
    if (!dwrite || !rt) return;
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 18.0f, L"en-us", &m_titleFormat);
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"en-us", &m_contentFormat);
    rt->CreateSolidColorBrush(D2D1::ColorF(0,0,0,0.5f), &m_overlay);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.12f,0.12f,0.14f,0.95f), &m_bgBrush);
    rt->CreateSolidColorBrush(style.textColor, &m_textBrush);
    m_closeButton.SetOnClick([this]() { Close(); });
}

void ModalDialog::SetTitle(const std::wstring& title) { m_title = title; }
void ModalDialog::SetContent(const std::wstring& content) { m_content = content; }
void ModalDialog::SetSize(float w, float h) { m_width = w; m_height = h; }

void ModalDialog::Show() {
    m_visible = true;
    m_alpha.SetTarget(1.0f);
}

void ModalDialog::Close() {
    m_visible = false;
    m_alpha.SetTarget(0.0f);
}

void ModalDialog::HandleClick(float mx, float my) {
    if (!m_visible) return;
    m_closeButton.SetHovered(m_closeButton.HitTest(mx, my));
}

void ModalDialog::Update(float dt) {
    m_alpha.Update(dt);
    m_closeButton.Update(dt);
}

void ModalDialog::Render(ID2D1RenderTarget* rt) {
    if (!m_visible) return;
    float a = m_alpha.GetValue();
    float cx = rt->GetSize().width / 2.0f;
    float cy = rt->GetSize().height / 2.0f;
    m_x = cx - m_width / 2.0f;
    m_y = cy - m_height / 2.0f;

    m_overlay->SetColor(D2D1::ColorF(0,0,0,0.5f * a));
    rt->FillRectangle(D2D1::RectF(0,0,rt->GetSize().width,rt->GetSize().height), m_overlay.Get());

    D2D1_ROUNDED_RECT glassRect = D2D1::RoundedRect(
        D2D1::RectF(m_x, m_y, m_x+m_width, m_y+m_height), 16.0f, 16.0f
    );
    rex::graphics::LiquidGlassBrush::RenderElevatedPanel(rt, glassRect, m_glassTokens, a);

    m_textBrush->SetColor(D2D1::ColorF(1,1,1,a));
    rt->DrawTextW(m_title.c_str(), (UINT32)m_title.size(), m_titleFormat.Get(),
        D2D1::RectF(m_x+16, m_y+12, m_x+m_width-40, m_y+44), m_textBrush.Get());

    rt->DrawTextW(m_content.c_str(), (UINT32)m_content.size(), m_contentFormat.Get(),
        D2D1::RectF(m_x+16, m_y+50, m_x+m_width-16, m_y+m_height-16), m_textBrush.Get());

    m_closeButton.SetPosition(m_x + m_width - 32, m_y + 8);
    m_closeButton.Render(rt);
}
