#include "WidgetsPage.h"
#include "modules/Widgets/models/WidgetDefinition.hpp"
#include "modules/Widgets/models/WidgetInstance.hpp"
#include <sstream>

WidgetsPage::WidgetsPage(ID2D1RenderTarget* rt, IDWriteFactory* dwrite,
                         const GlowStyle& style,
                         std::shared_ptr<rex::widgets::IWidgetService> widgetService)
    : PageRenderer(rt, dwrite, style),
      m_service(widgetService),
      m_toolbar(rt, dwrite, style),
      m_gallery(rt, dwrite, style, 3, 240, 180, 12),
      m_inspector(rt, dwrite, style),
      m_typeDropdown(rt, dwrite, style)
{
    m_toolbar.SetActions({
        {L"Add Widget", L"+", [this]() {
            if (m_service) m_service->createWidget("clock", 400, 300);
            RefreshWidgetList();
        }},
        {L"Remove", L"\u2715", [this]() {
            if (m_selectedIndex >= 0 && m_selectedIndex < (int)m_widgetItems.size()) {
                std::string instanceId = std::any_cast<std::string>(m_widgetItems[m_selectedIndex].userData);
                m_service->removeWidget(instanceId);
                m_selectedIndex = -1;
                RefreshWidgetList();
                RefreshInspector();
            }
        }},
    });

    m_typeDropdown.SetItems({L"Clock", L"Weather", L"System Monitor"});
    m_typeDropdown.SetSelected(0);
}

void WidgetsPage::SetBounds(float x, float y, float w, float h) {
    m_x = x; m_y = y; m_w = w; m_h = h;
}

void WidgetsPage::Update(float dt) {
    m_toolbar.Update(dt);
    m_gallery.Update(dt);
}

void WidgetsPage::Render(ID2D1RenderTarget* rt) {
    if (!rt || !m_service) return;
    float y = m_y + 40;

    m_toolbar.SetBounds(m_x, m_y, m_w);
    m_toolbar.Render(rt);

    float galleryW = m_w * 0.65f;
    m_gallery.SetBounds(m_x, y, galleryW, m_h - y);
    m_gallery.Render(rt);

    float inspectorX = m_x + galleryW + 10;
    float inspectorW = m_w - galleryW - 10;
    m_inspector.SetBounds(inspectorX, y, inspectorW);
    m_inspector.Render(rt);

    m_typeDropdown.SetBounds(inspectorX, y + 250, inspectorW);
    m_typeDropdown.Render(rt);
}

void WidgetsPage::HandleClick(float mx, float my) {
    m_toolbar.HitTest(mx, my);
    m_gallery.HitTest(mx, my);
}

void WidgetsPage::RefreshWidgetList() {
    m_widgetItems.clear();
    if (!m_service) { m_gallery.SetItems(m_widgetItems); return; }
    auto instances = m_service->getInstances();
    for (auto& inst : instances) {
        GalleryItem item;
        item.title = L"Widget " + std::wstring(inst.widgetId.begin(), inst.widgetId.end());
        item.subtitle = L"ID: " + std::wstring(inst.id.begin(), inst.id.end());
        item.userData = inst.id;
        m_widgetItems.push_back(item);
    }
    m_gallery.SetItems(m_widgetItems);
    m_gallery.SetOnItemClick([this](int idx, const GalleryItem&) {
        OnWidgetSelected(idx);
    });
}

void WidgetsPage::OnWidgetSelected(int index) {
    m_selectedIndex = index;
    RefreshInspector();
}

void WidgetsPage::RefreshInspector() {
    std::vector<PropertyRow> props;
    if (m_selectedIndex >= 0 && m_selectedIndex < (int)m_widgetItems.size()) {
        std::string instanceId = std::any_cast<std::string>(m_widgetItems[m_selectedIndex].userData);
        auto instances = m_service->getInstances();
        for (auto& inst : instances) {
            if (inst.id == instanceId) {
                props.push_back({L"ID", std::wstring(inst.id.begin(), inst.id.end())});
                props.push_back({L"Type", std::wstring(inst.widgetId.begin(), inst.widgetId.end())});
                props.push_back({L"X", std::to_wstring(inst.x)});
                props.push_back({L"Y", std::to_wstring(inst.y)});
                props.push_back({L"Visible", inst.visible ? L"Yes" : L"No"});
                break;
            }
        }
    } else {
        props.push_back({L"Info", L"Select a widget"});
    }
    m_inspector.SetProperties(props);
}
