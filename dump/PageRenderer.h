#pragma once
#include <d2d1_3.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <vector>
#include <functional>
#include <string>
#include "GlowStyle.h"
#include "GlowTextButton.h"
#include "GlowTextInput.h"

class PageRenderer {
public:
    PageRenderer(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, const GlowStyle& style);
    virtual ~PageRenderer() = default;
    virtual void Update(float dt);
    virtual void Render(ID2D1RenderTarget* rt);
    virtual void UpdateHover(float mx, float my);
    virtual void HandleClick(float mx, float my);
    virtual void HandleChar(wchar_t ch);
    virtual void HandleKeyDown(int key);

protected:
    void AddButton(const std::wstring& text, float x, float y,
                   std::function<void()> onClick = nullptr);
    void AddInput(const std::wstring& initialText, float x, float y, float width,
                  std::function<void(const std::wstring&)> onChange = nullptr);

    ID2D1RenderTarget* m_rt;
    IDWriteFactory* m_dwrite;
    GlowStyle m_style;
    std::vector<GlowTextButton> m_buttons;
    std::vector<GlowTextInput> m_inputs;
    int m_focusedInput = -1;
};
