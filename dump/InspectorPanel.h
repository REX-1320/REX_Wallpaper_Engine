#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include "shell/GlowStyle.h"
#include "shell/PropertyGrid.h"

namespace rex::shell {

class InspectorPanel {
public:
    InspectorPanel();
    void Init(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, const GlowStyle& style);
    void SetBounds(float x, float y, float w, float h);
    void Update(float dt);
    void Render(ID2D1RenderTarget* rt);
    void HandleClick(float mx, float my);

    PropertyGrid& GetPropertyGrid() { return m_propertyGrid; }

private:
    PropertyGrid m_propertyGrid;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_headerFormat;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_headerBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_edgeBrush;
    float m_x = 0, m_y = 0, m_w = 0, m_h = 0;
};

}
