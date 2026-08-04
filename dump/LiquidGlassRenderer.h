#pragma once
#include <d2d1.h>
#include <wrl/client.h>
#include "shell/LiquidGlassTokens.hpp"

namespace rex::shell {

struct LayoutRects {
    D2D1_RECT_F topBar;
    D2D1_RECT_F sidebar;
    D2D1_RECT_F workspace;
    D2D1_RECT_F inspector;
    D2D1_RECT_F statusBar;
};

class LiquidGlassRenderer {
public:
    LiquidGlassRenderer();
    void Init(ID2D1RenderTarget* rt);
    void RenderGlass(ID2D1RenderTarget* rt, const LayoutRects* layout);

    void SetTokens(const LiquidGlassTokens& tokens) { m_tokens = tokens; }
    const LiquidGlassTokens& GetTokens() const { return m_tokens; }

private:
    LiquidGlassTokens m_tokens;

    void ApplyGlassToRegion(ID2D1RenderTarget* rt, const D2D1_RECT_F& region,
                            const GlassLayer& layer, const GlassEdgeHighlight& edge);
};

}
