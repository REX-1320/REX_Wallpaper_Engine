#include "shell/LiquidGlassRenderer.h"

namespace rex::shell {

LiquidGlassRenderer::LiquidGlassRenderer() {
    m_tokens = LiquidGlassTokens::DefaultDark();
}

void LiquidGlassRenderer::Init(ID2D1RenderTarget* rt) {
    (void)rt;
}

void LiquidGlassRenderer::RenderGlass(ID2D1RenderTarget* rt, const LayoutRects* layout) {
    if (!rt || !layout) return;

    ApplyGlassToRegion(rt, layout->topBar, m_tokens.surface, m_tokens.edgeHighlight);
    ApplyGlassToRegion(rt, layout->sidebar, m_tokens.base, m_tokens.edgeHighlight);
    ApplyGlassToRegion(rt, layout->workspace, m_tokens.surface, m_tokens.edgeHighlight);
    ApplyGlassToRegion(rt, layout->inspector, m_tokens.surface, m_tokens.edgeHighlight);
    ApplyGlassToRegion(rt, layout->statusBar, m_tokens.surface, m_tokens.edgeHighlight);
}

void LiquidGlassRenderer::ApplyGlassToRegion(ID2D1RenderTarget* rt,
                                              const D2D1_RECT_F& region,
                                              const GlassLayer& layer,
                                              const GlassEdgeHighlight& edge) {
    float w = region.right - region.left;
    float h = region.bottom - region.top;
    if (w < 1 || h < 1) return;

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> tintBrush;
    rt->CreateSolidColorBrush(layer.tint, &tintBrush);
    if (tintBrush) {
        rt->FillRectangle(region, tintBrush.Get());
    }

    if (edge.color.a > 0.001f && edge.width > 0.0f) {
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> edgeBrush;
        rt->CreateSolidColorBrush(edge.color, &edgeBrush);
        if (edgeBrush) {
            rt->DrawLine(D2D1::Point2F(region.left, region.top),
                D2D1::Point2F(region.right, region.top), edgeBrush.Get(), edge.width);
            rt->DrawLine(D2D1::Point2F(region.left, region.top),
                D2D1::Point2F(region.left, region.bottom), edgeBrush.Get(), edge.width);
        }
    }
}

}
