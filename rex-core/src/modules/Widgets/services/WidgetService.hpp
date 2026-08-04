#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <atomic>

#include "../interfaces/IWidgetService.hpp"
#include "../models/WidgetInstance.hpp"
#include "WidgetRegistry.hpp"
#include "WidgetHost.hpp"

namespace rex::widgets {

class IWidgetService;

class WidgetService : public IWidgetService {
public:
    WidgetService(std::shared_ptr<WidgetRegistry> registry,
                  std::shared_ptr<WidgetHost> host);
    ~WidgetService() = default;

    std::string createWidget(const std::string& widgetDefId, int x, int y) override;
    void removeWidget(const std::string& instanceId) override;
    void moveWidget(const std::string& instanceId, int x, int y) override;
    void setVisible(const std::string& instanceId, bool visible) override;
    std::vector<WidgetInstance> getInstances() const override;

private:
    std::shared_ptr<WidgetRegistry> m_registry;
    std::shared_ptr<WidgetHost> m_host;

    mutable std::mutex m_mutex;
    std::vector<WidgetInstance> m_instances;
    std::atomic<int> m_nextId{1};

    std::string makeInstanceId();
};

} // namespace rex::widgets
