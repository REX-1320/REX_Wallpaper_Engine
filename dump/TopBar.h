#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <memory>
#include "shell/GlowStyle.h"
#include "shell/SearchBar.h"

class ID2D1RenderTarget;

namespace rex::shell {

class TopBar {
public:
    TopBar();
    void Init(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, const GlowStyle& style);
    void SetBounds(float x, float y, float w, float h);
    void Update(float dt);
    void Render(ID2D1RenderTarget* rt);
    void HandleHover(float mx, float my);
    void HandleClick(float mx, float my);

    SearchBar& GetSearchBar() { return m_searchBar; }

private:
    SearchBar m_searchBar;

    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_titleFormat;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_titleBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_edgeBrush;

    float m_x = 0, m_y = 0, m_w = 0, m_h = 0;
    bool m_initialized = false;
};

}
