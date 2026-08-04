#pragma once
#include <d2d1.h>

namespace rex::shell {

struct LayoutConfig {
    float sidebarWidth;
    float inspectorWidth;
    bool sidebarCollapsed;
    bool inspectorHidden;
};

class LayoutManager {
public:
    LayoutManager();

    void SetWindowSize(int width, int height);
    void ToggleSidebar();
    void ToggleInspector();
    void SetSidebarCollapsed(bool collapsed);
    void SetInspectorHidden(bool hidden);

    const LayoutConfig& GetConfig() const { return m_config; }
    D2D1_RECT_F GetTopBarRect() const;
    D2D1_RECT_F GetSidebarRect() const;
    D2D1_RECT_F GetWorkspaceRect() const;
    D2D1_RECT_F GetInspectorRect() const;
    D2D1_RECT_F GetStatusBarRect() const;

private:
    int m_width = 0, m_height = 0;
    LayoutConfig m_config;
    static constexpr float TOP_BAR_HEIGHT = 48.0f;
    static constexpr float STATUS_BAR_HEIGHT = 28.0f;

    void ComputeBreakpoints();
};

}
