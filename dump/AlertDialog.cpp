#include "AlertDialog.h"

AlertDialog::AlertDialog(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, const GlowStyle& style)
    : m_okButton(L"OK", rt, dwrite, style), m_style(style) {
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"en-us", &m_format);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.12f,0.12f,0.14f,0.95f), &m_bgBrush);
    rt->CreateSolidColorBrush(style.textColor, &m_textBrush);
    m_okButton.SetOnClick([this]() { m_visible = false; if (m_onOk) m_onOk(); });
}

void AlertDialog::Show(const std::wstring& message, std::function<void()> onOk) {
    m_message = message;
    m_onOk = onOk;
    m_visible = true;
}

void AlertDialog::HandleClick(float mx, float my) {
    if (!m_visible) return;
    m_okButton.SetHovered(m_okButton.HitTest(mx, my));
}

void AlertDialog::Render(ID2D1RenderTarget* rt) {
    if (!m_visible) return;
    float cx = rt->GetSize().width / 2.0f;
    float cy = rt->GetSize().height / 2.0f;
    m_x = cx - m_width / 2.0f;
    m_y = cy - m_height / 2.0f;
    D2D1_RECT_F bg = {m_x, m_y, m_x+m_width, m_y+m_height};
    rt->FillRoundedRectangle(D2D1::RoundedRect(bg, 12, 12), m_bgBrush.Get());
    rt->DrawTextW(m_message.c_str(), (UINT32)m_message.size(), m_format.Get(),
        D2D1::RectF(m_x+16, m_y+20, m_x+m_width-16, m_y+m_height-50), m_textBrush.Get());
    m_okButton.SetPosition(m_x + m_width/2 - 40, m_y + m_height - 40);
    m_okButton.Render(rt);
}
