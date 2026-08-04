#pragma once
#include <vector>
#include <string>
#include <functional>
#include <any>
#include <d2d1_3.h>
#include <dwrite.h>
#include <wrl/client.h>
#include "GlowStyle.h"
#include "SpringAnimation.h"

struct TreeNode {
    std::wstring label;
    std::vector<TreeNode> children;
    bool expanded = false;
    std::any userData;
};

class TreeView {
public:
    TreeView(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, const GlowStyle& style);
    void SetBounds(float x, float y, float width, float height);
    void SetRoot(const TreeNode& root);
    void SetOnNodeClick(std::function<void(const TreeNode&)> cb) { m_onNodeClick = std::move(cb); }

    bool HitTest(float mx, float my);
    void Update(float dt);
    void Render(ID2D1RenderTarget* rt);

private:
    float m_x, m_y, m_width, m_height;
    TreeNode m_root;
    int m_hoveredIndex = -1;

    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_textFormat;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_textBrush, m_hoverBrush, m_chevronBrush;
    GlowStyle m_style;
    std::function<void(const TreeNode&)> m_onNodeClick;

    void RenderNode(ID2D1RenderTarget* rt, const TreeNode& node, float& y, int depth);
    int HitTestNode(const TreeNode& node, float mx, float my, float& y, int depth, int& index);
};
