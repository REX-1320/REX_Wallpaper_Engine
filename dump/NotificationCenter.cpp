#include "NotificationCenter.h"
#include "modules/Notification/interfaces/INotificationService.hpp"
#include <sstream>
#include <iomanip>
#include <ctime>

NotificationCenter::NotificationCenter(ID2D1RenderTarget* rt, IDWriteFactory* dwrite,
                                       const GlowStyle& style,
                                       std::shared_ptr<rex::notifications::INotificationService> service)
    : m_service(service),
      m_clearAllBtn(L"Clear All", rt, dwrite, style)
{
    if (!dwrite || !rt) return;
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"en-us", &m_titleFormat);
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-us", &m_bodyFormat);
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 10.0f, L"en-us", &m_timeFormat);

    rt->CreateSolidColorBrush(D2D1::ColorF(0.12f,0.12f,0.14f,0.95f), &m_bgBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.3f,0.3f,0.3f,0.8f), &m_borderBrush);
    rt->CreateSolidColorBrush(style.textColor, &m_textBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.6f,0.6f,0.6f,1.0f), &m_secondaryBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.2f,0.2f,0.2f,0.5f), &m_dividerBrush);

    m_clearAllBtn.SetOnClick([this]() {
        if (m_service) m_service->clearHistory();
    });
}

void NotificationCenter::SetBounds(float x, float y, float w, float h) {
    m_x = x; m_y = y; m_w = w; m_h = h;
}

void NotificationCenter::Show() {
    m_visible = true;
    m_slideSpring.SetTarget(1.0f);
    m_fadeSpring.SetTarget(1.0f);
}

void NotificationCenter::Hide() {
    m_slideSpring.SetTarget(0.0f);
    m_fadeSpring.SetTarget(0.0f);
}

void NotificationCenter::Toggle() {
    if (m_visible) Hide(); else Show();
}

void NotificationCenter::Update(float dt) {
    m_slideSpring.Update(dt);
    m_fadeSpring.Update(dt);
    m_clearAllBtn.Update(dt);
    if (m_slideSpring.IsResting() && m_slideSpring.GetTarget() == 0.0f)
        m_visible = false;
}

void NotificationCenter::Render(ID2D1RenderTarget* rt) {
    if (!m_visible && m_slideSpring.IsResting()) return;
    float slide = m_slideSpring.GetValue();
    float fade = m_fadeSpring.GetValue();

    float offsetX = (1.0f - slide) * m_w;
    float panelX = m_x + offsetX;
    float panelY = m_y;
    float panelW = m_w;
    float panelH = m_h;

    auto panel = D2D1::RectF(panelX, panelY, panelX+panelW, panelY+panelH);
    auto bg = D2D1::ColorF(0.12f,0.12f,0.14f,0.95f * fade);
    m_bgBrush->SetColor(bg);
    rt->FillRoundedRectangle(D2D1::RoundedRect(panel, 8, 8), m_bgBrush.Get());

    auto border = D2D1::ColorF(0.3f,0.3f,0.3f,0.8f * fade);
    m_borderBrush->SetColor(border);
    rt->DrawRoundedRectangle(D2D1::RoundedRect(panel, 8, 8), m_borderBrush.Get(), 1);

    auto titleRect = D2D1::RectF(panelX + 12, panelY + 8, panelX+panelW-12, panelY + 30);
    rt->DrawTextW(L"Notifications", 13, m_titleFormat.Get(), titleRect, m_textBrush.Get());

    m_clearAllBtn.SetPosition(panelX + panelW - 80, panelY + 6);
    m_clearAllBtn.Render(rt);

    float itemY = panelY + 40;
    if (m_service) {
        auto history = m_service->getHistory();
        int count = 0;
        for (auto& n : history) {
            if (itemY > panelY + panelH - 10) break;
            rt->DrawLine(D2D1::Point2F(panelX+12, itemY), D2D1::Point2F(panelX+panelW-12, itemY), m_dividerBrush.Get(), 1);
            itemY += 8;
            std::wstring title(n.title.begin(), n.title.end());
            rt->DrawTextW(title.c_str(), (UINT32)title.size(), m_titleFormat.Get(),
                D2D1::RectF(panelX+12, itemY, panelX+panelW-12, itemY+18), m_textBrush.Get());
            itemY += 18;
            std::wstring msg(n.message.begin(), n.message.end());
            rt->DrawTextW(msg.c_str(), (UINT32)msg.size(), m_bodyFormat.Get(),
                D2D1::RectF(panelX+12, itemY, panelX+panelW-12, itemY+30), m_secondaryBrush.Get());
            itemY += 32;
            ++count;
        }
        if (count == 0) {
            rt->DrawTextW(L"No notifications", 16, m_bodyFormat.Get(),
                D2D1::RectF(panelX+12, itemY, panelX+panelW-12, itemY+20), m_secondaryBrush.Get());
        }
    }
}

bool NotificationCenter::HitTest(float mx, float my) {
    return m_visible && mx >= m_x && mx <= m_x + m_w && my >= m_y && my <= m_y + m_h;
}

void NotificationCenter::HandleClick(float mx, float my) {
    m_clearAllBtn.HitTest(mx, my);
}
