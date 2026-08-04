#pragma once
#include <vector>
#include <functional>
#include <d2d1_3.h>
#include <dwrite.h>
#include <wrl/client.h>
#include "Button.h"
#include "GlowStyle.h"

struct ToolbarAction {
    std::wstring label;
    std::wstring icon;
    std::function<void()> action;
    bool enabled = true;
};

class Toolbar {
public:
    Toolbar(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, const GlowStyle& style);
    void SetBounds(float x, float y, float width);
    void SetActions(const std::vector<ToolbarAction>& actions);
    bool HitTest(float mx, float my);
    void Update(float dt);
    void Render(ID2D1RenderTarget* rt);

private:
    float m_x, m_y, m_width;
    float m_itemSize = 32;
    std::vector<ToolbarAction> m_actions;
    std::vector<Button> m_buttons;
    GlowStyle m_style;
    ID2D1RenderTarget* m_rt;
    IDWriteFactory* m_dwrite;
};
