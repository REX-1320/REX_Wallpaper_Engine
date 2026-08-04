#include "SearchBar.h"

SearchBar::SearchBar(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, const GlowStyle& style)
    : m_input(0, 0, 400, rt, dwrite, style),
      m_clearButton(L"\u00D7", rt, dwrite, style), m_style(style) {}

void SearchBar::SetPosition(float x, float y, float width) {
    m_input.SetPosition(x, y);
    m_input.SetWidth(width - 30);
    m_clearButton.SetPosition(x + width - 25, y + 2);
    m_clearButton.SetSize(20, 20);
}

bool SearchBar::HitTest(float mx, float my) {
    return m_input.HitTest(mx, my) || m_clearButton.HitTest(mx, my);
}

void SearchBar::HandleChar(wchar_t ch) { m_input.SetText(m_input.GetText() + ch); }

void SearchBar::HandleKeyDown(int key) {
    if (key == VK_BACK) {
        std::wstring txt = m_input.GetText();
        if (!txt.empty()) { txt.pop_back(); m_input.SetText(txt); }
    }
}

void SearchBar::Update(float dt) { m_input.Update(dt); m_clearButton.Update(dt); }

void SearchBar::Render(ID2D1RenderTarget* rt) {
    m_input.Render(rt);
    m_clearButton.Render(rt);
}
