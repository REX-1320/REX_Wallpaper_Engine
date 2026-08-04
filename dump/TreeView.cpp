#include "TreeView.h"

TreeView::TreeView(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, const GlowStyle& style)
    : m_style(style) {
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"en-us", &m_textFormat);
    rt->CreateSolidColorBrush(style.textColor, &m_textBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(style.glowColor.r, style.glowColor.g,
        style.glowColor.b, 0.3f), &m_hoverBrush);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.6f,0.6f,0.6f,1.0f), &m_chevronBrush);
}

void TreeView::SetBounds(float x, float y, float width, float height) {
    m_x = x; m_y = y; m_width = width; m_height = height;
}

void TreeView::SetRoot(const TreeNode& root) { m_root = root; }

void TreeView::Update(float dt) {}

bool TreeView::HitTest(float mx, float my) {
    float y = m_y;
    int index = 0;
    return HitTestNode(m_root, mx, my, y, 0, index) >= 0;
}

int TreeView::HitTestNode(const TreeNode& node, float mx, float my, float& y, int depth, int& index) {
    int curIndex = index++;
    if (mx >= m_x && mx <= m_x + m_width && my >= y && my <= y + 24) {
        m_hoveredIndex = curIndex;
        return curIndex;
    }
    y += 24;
    if (node.expanded) {
        for (auto& child : node.children) {
            int result = HitTestNode(child, mx, my, y, depth + 1, index);
            if (result >= 0) return result;
        }
    }
    return -1;
}

void TreeView::Render(ID2D1RenderTarget* rt) {
    float y = m_y;
    RenderNode(rt, m_root, y, 0);
}

void TreeView::RenderNode(ID2D1RenderTarget* rt, const TreeNode& node, float& y, int depth) {
    float x = m_x + depth * 20.0f;
    std::wstring chevron = node.children.empty() ? L"  " : (node.expanded ? L"\u25BE " : L"\u25B8 ");
    rt->DrawTextW(chevron.c_str(), (UINT32)chevron.size(), m_textFormat.Get(),
        D2D1::RectF(x, y, x+20, y+22), m_chevronBrush.Get());
    rt->DrawTextW(node.label.c_str(), (UINT32)node.label.size(), m_textFormat.Get(),
        D2D1::RectF(x+16, y, x+m_width, y+22), m_textBrush.Get());
    y += 24;
    if (node.expanded) {
        for (auto& child : node.children) {
            RenderNode(rt, child, y, depth+1);
        }
    }
}
