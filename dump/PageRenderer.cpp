#include "PageRenderer.h"
#include <algorithm>

PageRenderer::PageRenderer(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, const GlowStyle& style)
    : m_rt(rt), m_dwrite(dwrite), m_style(style) {}

void PageRenderer::UpdateHover(float mx, float my) {
    for (auto& btn : m_buttons) btn.SetHovered(btn.HitTest(mx, my));
    for (auto& inp : m_inputs) inp.SetFocused(inp.HitTest(mx, my));
}

void PageRenderer::Update(float dt) {
    for (auto& btn : m_buttons) btn.Update(dt);
    for (auto& inp : m_inputs) inp.Update(dt);
}

void PageRenderer::Render(ID2D1RenderTarget* rt) {
    for (auto& btn : m_buttons) btn.Render(rt);
    for (size_t i = 0; i < m_inputs.size(); ++i) {
        m_inputs[i].SetFocused(i == (size_t)m_focusedInput);
        m_inputs[i].Render(rt);
    }
}

void PageRenderer::HandleClick(float mx, float my) {
    for (auto& btn : m_buttons) {
        if (btn.HitTest(mx, my)) {
            auto cb = btn.GetOnClick();
            if (cb) cb();
            return;
        }
    }
    m_focusedInput = -1;
    for (size_t i = 0; i < m_inputs.size(); ++i) {
        if (m_inputs[i].HitTest(mx, my)) {
            m_focusedInput = (int)i;
            return;
        }
    }
}

void PageRenderer::HandleChar(wchar_t ch) {
    if (m_focusedInput >= 0 && m_focusedInput < (int)m_inputs.size()) {
        auto& inp = m_inputs[m_focusedInput];
        inp.SetText(inp.GetText() + ch);
    }
}

void PageRenderer::HandleKeyDown(int key) {
    if (m_focusedInput >= 0 && m_focusedInput < (int)m_inputs.size()) {
        auto& inp = m_inputs[m_focusedInput];
        if (key == VK_BACK) {
            std::wstring txt = inp.GetText();
            if (!txt.empty()) txt.pop_back();
            inp.SetText(txt);
        }
    }
}

void PageRenderer::AddButton(const std::wstring& text, float x, float y,
                             std::function<void()> onClick) {
    m_buttons.emplace_back(text, m_rt, m_dwrite, m_style);
    auto& btn = m_buttons.back();
    btn.SetPosition(x, y);
    btn.SetFontSize(24.0f);
    if (onClick) btn.SetOnClick(std::move(onClick));
}

void PageRenderer::AddInput(const std::wstring& initialText, float x, float y, float width,
                            std::function<void(const std::wstring&)> onChange) {
    m_inputs.emplace_back(x, y, width, m_rt, m_dwrite, m_style);
    auto& inp = m_inputs.back();
    inp.SetText(initialText);
}
