#include "shell/WorkspaceHost.h"

namespace rex::shell {

WorkspaceHost::WorkspaceHost() {}

void WorkspaceHost::Init(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, const GlowStyle& style) {
    if (!dwrite || !rt) return;
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        16.0f, L"en-us", &m_placeholderFormat);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.5f, 0.5f, 0.5f, 1.0f), &m_placeholderBrush);
}

void WorkspaceHost::SetBounds(float x, float y, float w, float h) {
    m_x = x; m_y = y; m_w = w; m_h = h;
    if (m_page) m_page->SetBounds(m_x, m_y, m_w, m_h);
}

void WorkspaceHost::Update(float dt) {
    if (m_page) m_page->Update(dt);
}

void WorkspaceHost::Render(ID2D1RenderTarget* rt) {
    if (!rt) return;
    if (m_page) {
        m_page->Render(rt);
    } else {
        rt->DrawTextW(L"Select a page from the sidebar", 31, m_placeholderFormat.Get(),
            D2D1::RectF(m_x + 16, m_y + 16, m_x + m_w - 16, m_y + m_h - 16),
            m_placeholderBrush.Get());
    }
}

void WorkspaceHost::HandleClick(float mx, float my) {
    if (m_page) m_page->HandleClick(mx, my);
}

void WorkspaceHost::SetPage(std::unique_ptr<PageRenderer> page) {
    m_page = std::move(page);
    if (m_page) m_page->SetBounds(m_x, m_y, m_w, m_h);
}

}
