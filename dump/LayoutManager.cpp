#include "shell/LayoutManager.h"
#include <algorithm>

namespace rex::shell {

LayoutManager::LayoutManager() {
    m_config.sidebarWidth = 260.0f;
    m_config.inspectorWidth = 320.0f;
    m_config.sidebarCollapsed = false;
    m_config.inspectorHidden = false;
}

void LayoutManager::SetWindowSize(int width, int height) {
    m_width = width;
    m_height = height;
    ComputeBreakpoints();
}

void LayoutManager::ToggleSidebar() {
    m_config.sidebarCollapsed = !m_config.sidebarCollapsed;
    ComputeBreakpoints();
}

void LayoutManager::ToggleInspector() {
    m_config.inspectorHidden = !m_config.inspectorHidden;
    ComputeBreakpoints();
}

void LayoutManager::SetSidebarCollapsed(bool collapsed) {
    m_config.sidebarCollapsed = collapsed;
    ComputeBreakpoints();
}

void LayoutManager::SetInspectorHidden(bool hidden) {
    m_config.inspectorHidden = hidden;
    ComputeBreakpoints();
}

void LayoutManager::ComputeBreakpoints() {
    float w = (float)m_width;

    if (w < 1366.0f) {
        m_config.sidebarWidth = 64.0f;
        m_config.inspectorWidth = 0.0f;
        m_config.inspectorHidden = true;
    } else if (w <= 1440.0f) {
        m_config.sidebarWidth = m_config.sidebarCollapsed ? 64.0f : 240.0f;
        m_config.inspectorWidth = 280.0f;
    } else if (w <= 1600.0f) {
        m_config.sidebarWidth = m_config.sidebarCollapsed ? 64.0f : 260.0f;
        m_config.inspectorWidth = 320.0f;
    } else if (w <= 1920.0f) {
        m_config.sidebarWidth = m_config.sidebarCollapsed ? 64.0f : 260.0f;
        m_config.inspectorWidth = 320.0f;
    } else {
        m_config.sidebarWidth = m_config.sidebarCollapsed ? 64.0f : 280.0f;
        m_config.inspectorWidth = 360.0f;
    }

    if (m_config.inspectorHidden) m_config.inspectorWidth = 0.0f;
}

D2D1_RECT_F LayoutManager::GetTopBarRect() const {
    return D2D1::RectF(0, 0, (float)m_width, TOP_BAR_HEIGHT);
}

D2D1_RECT_F LayoutManager::GetSidebarRect() const {
    return D2D1::RectF(0, TOP_BAR_HEIGHT, m_config.sidebarWidth, (float)m_height - STATUS_BAR_HEIGHT);
}

D2D1_RECT_F LayoutManager::GetWorkspaceRect() const {
    return D2D1::RectF(m_config.sidebarWidth, TOP_BAR_HEIGHT,
        (float)m_width - m_config.inspectorWidth, (float)m_height - STATUS_BAR_HEIGHT);
}

D2D1_RECT_F LayoutManager::GetInspectorRect() const {
    return D2D1::RectF((float)m_width - m_config.inspectorWidth, TOP_BAR_HEIGHT,
        (float)m_width, (float)m_height - STATUS_BAR_HEIGHT);
}

D2D1_RECT_F LayoutManager::GetStatusBarRect() const {
    return D2D1::RectF(0, (float)m_height - STATUS_BAR_HEIGHT, (float)m_width, (float)m_height);
}

}
