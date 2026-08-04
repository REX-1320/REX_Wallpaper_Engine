#include "WidgetService.hpp"

#include <algorithm>

namespace rex::widgets {

WidgetService::WidgetService(std::shared_ptr<WidgetRegistry> registry,
                             std::shared_ptr<WidgetHost> host)
    : m_registry(registry), m_host(host) {
}

std::string WidgetService::createWidget(const std::string& widgetDefId, int x, int y) {
    auto* def = m_registry->get(widgetDefId);
    if (!def) return {};

    std::lock_guard<std::mutex> lock(m_mutex);

    WidgetInstance inst;
    inst.id = makeInstanceId();
    inst.widgetId = widgetDefId;
    inst.x = x;
    inst.y = y;
    inst.width = def->defaultWidth;
    inst.height = def->defaultHeight;

    if (m_host->createWindow(inst, *def)) {
        m_instances.push_back(inst);
        return inst.id;
    }
    return {};
}

void WidgetService::removeWidget(const std::string& instanceId) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = std::find_if(m_instances.begin(), m_instances.end(),
        [&](const WidgetInstance& i) { return i.id == instanceId; });

    if (it != m_instances.end()) {
        m_host->destroyWindow(instanceId);
        m_instances.erase(it);
    }
}

void WidgetService::moveWidget(const std::string& instanceId, int x, int y) {
    std::lock_guard<std::mutex> lock(m_mutex);

    for (auto& inst : m_instances) {
        if (inst.id == instanceId) {
            inst.x = x;
            inst.y = y;
            m_host->moveWindow(instanceId, x, y);
            return;
        }
    }
}

void WidgetService::setVisible(const std::string& instanceId, bool visible) {
    std::lock_guard<std::mutex> lock(m_mutex);

    for (auto& inst : m_instances) {
        if (inst.id == instanceId) {
            inst.visible = visible;
            m_host->setVisible(instanceId, visible);
            return;
        }
    }
}

std::vector<WidgetInstance> WidgetService::getInstances() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_instances;
}

std::string WidgetService::makeInstanceId() {
    return "widget_" + std::to_string(m_nextId++);
}

} // namespace rex::widgets
