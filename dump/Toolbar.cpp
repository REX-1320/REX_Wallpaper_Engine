#include "Toolbar.h"

Toolbar::Toolbar(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, const GlowStyle& style)
    : m_rt(rt), m_dwrite(dwrite), m_style(style) {}

void Toolbar::SetBounds(float x, float y, float width) { m_x = x; m_y = y; m_width = width; }

void Toolbar::SetActions(const std::vector<ToolbarAction>& actions) {
    m_actions = actions;
    m_buttons.clear();
    float x = m_x;
    for (auto& action : actions) {
        Button btn(action.icon + L" " + action.label, m_rt, m_dwrite, m_style);
        btn.SetPosition(x, m_y);
        btn.SetSize(m_itemSize, m_itemSize);
        btn.SetOnClick(action.action);
        m_buttons.push_back(std::move(btn));
        x += m_itemSize + 4;
    }
}

bool Toolbar::HitTest(float mx, float my) {
    for (auto& btn : m_buttons)
        if (btn.HitTest(mx, my)) return true;
    return false;
}

void Toolbar::Update(float dt) {
    for (auto& btn : m_buttons) btn.Update(dt);
}

void Toolbar::Render(ID2D1RenderTarget* rt) {
    for (auto& btn : m_buttons) btn.Render(rt);
}
