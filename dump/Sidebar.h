#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <vector>
#include <string>
#include "shell/GlowStyle.h"
#include "shell/GlowTextButton.h"

namespace rex::shell {

class Sidebar {
public:
    Sidebar();
    void Init(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, const GlowStyle& style);
    void SetBounds(float x, float y, float w, float h);
    void Update(float dt);
    void Render(ID2D1RenderTarget* rt);
    void HandleHover(float mx, float my);
    void HandleClick(float mx, float my);
    void SetHoveredItem(float mx, float my);
    void SetActivePage(int page);
    int GetActivePage() const { return m_activePage; }

private:
    struct GroupDef {
        std::wstring header;
        std::vector<std::wstring> items;
    };

    std::vector<GroupDef> m_groups;
    std::vector<GlowTextButton> m_buttons;
    std::vector<int> m_itemToFlatIdx;
    int m_activePage = 0;
    int m_hoveredItem = -1;

    float m_x = 0, m_y = 0, m_w = 0, m_h = 0;

    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_headerFormat;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_edgeBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_accentBrush;

    bool m_initialized = false;

    void BuildItems();
    void UpdateSelection();
};

}
