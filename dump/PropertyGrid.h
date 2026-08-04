#pragma once
#include <vector>
#include <string>
#include <d2d1_3.h>
#include <dwrite.h>
#include <wrl/client.h>
#include "GlowStyle.h"
#include "GlowTextInput.h"

struct PropertyRow {
    std::wstring label;
    std::wstring value;
    bool editable = false;
};

class PropertyGrid {
public:
    PropertyGrid(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, const GlowStyle& style);
    void SetBounds(float x, float y, float width);
    void SetProperties(const std::vector<PropertyRow>& props);
    void Render(ID2D1RenderTarget* rt);

private:
    float m_x, m_y, m_width;
    std::vector<PropertyRow> m_props;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_labelFormat, m_valueFormat;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_labelBrush, m_valueBrush;
    GlowStyle m_style;
};
