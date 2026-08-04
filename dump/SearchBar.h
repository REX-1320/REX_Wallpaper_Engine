#pragma once
#include "GlowTextInput.h"
#include "Button.h"
#include <functional>

class SearchBar {
public:
    SearchBar(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, const GlowStyle& style);
    void SetPosition(float x, float y, float width);
    void SetOnSearch(std::function<void(const std::wstring&)> cb) { m_onSearch = std::move(cb); }
    std::wstring GetText() const { return m_input.GetText(); }
    void SetFocused(bool focused) { m_input.SetFocused(focused); }
    bool HitTest(float mx, float my);
    void HandleChar(wchar_t ch);
    void HandleKeyDown(int key);
    void Update(float dt);
    void Render(ID2D1RenderTarget* rt);

private:
    GlowTextInput m_input;
    Button m_clearButton;
    std::function<void(const std::wstring&)> m_onSearch;
    GlowStyle m_style;
};
