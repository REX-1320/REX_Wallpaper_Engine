#pragma once
#include <vector>
#include <string>
#include <functional>
#include <d2d1_3.h>
#include <dwrite.h>
#include <wrl/client.h>
#include "GlowStyle.h"
#include "ProgressBar.h"

struct StatusItem {
    std::wstring text;
    std::function<void()> onClick;
};

class StatusBar {
public:
    StatusBar(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, const GlowStyle& style);
    void SetBounds(float x, float y, float width);
    void SetLeftItems(const std::vector<StatusItem>& items);
    void SetRightItems(const std::vector<StatusItem>& items);
    void SetProgress(float p);
    void Render(ID2D1RenderTarget* rt);

private:
    float m_x, m_y, m_width;
    float m_height = 26;
    std::vector<StatusItem> m_leftItems, m_rightItems;
    ProgressBar m_progressBar;

    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_textFormat;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_bgBrush, m_textBrush;
    GlowStyle m_style;
};
