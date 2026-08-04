#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <memory>
#include "shell/PageRenderer.h"

namespace rex::shell {

class WorkspaceHost {
public:
    WorkspaceHost();
    void Init(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, const GlowStyle& style);
    void SetBounds(float x, float y, float w, float h);
    void Update(float dt);
    void Render(ID2D1RenderTarget* rt);
    void HandleClick(float mx, float my);

    void SetPage(std::unique_ptr<PageRenderer> page);
    PageRenderer* GetPage() const { return m_page.get(); }

private:
    std::unique_ptr<PageRenderer> m_page;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_placeholderFormat;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_placeholderBrush;
    float m_x = 0, m_y = 0, m_w = 0, m_h = 0;
};

}
