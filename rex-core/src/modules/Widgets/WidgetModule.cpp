#include "WidgetModule.hpp"

#include "services/WidgetRegistry.hpp"
#include "services/WidgetHost.hpp"
#include "services/WidgetService.hpp"

#include "modules/ModuleContext.hpp"
#include "modules/ServiceContainer.hpp"

namespace rex::widgets {

WidgetModule::WidgetModule() {
    metadata_.id = "rex.widgets";
    metadata_.name = "Widget Platform";
    metadata_.version = "1.0.0";
    metadata_.author = "REX Team";
    metadata_.description = "Manages desktop widgets (Clock, Weather, etc.)";
    metadata_.category = "Core";
    metadata_.builtIn = true;
    metadata_.autoStart = true;
    metadata_.enabledByDefault = true;
    metadata_.dependencies = {"rex.core"};
}

bool WidgetModule::initialize(rex::modules::IModuleContext* context) {
    auto registry = std::make_shared<WidgetRegistry>();

    registry->registerDefinition({
        "clock", "Clock", "1.0", "REX", "html",
        "modules/Widgets/widgets/clock/clock.html", "",
        180, 180
    });
    registry->registerDefinition({
        "weather", "Weather", "1.0", "REX", "html",
        "modules/Widgets/widgets/weather/weather.html", "",
        180, 140
    });
    registry->registerDefinition({
        "sysmon", "System Monitor", "1.0", "REX", "html",
        "modules/Widgets/widgets/sysmon/sysmon.html", "",
        220, 180
    });

    auto host = std::make_shared<WidgetHost>(registry);
    host->setOnWidgetClose([this](const std::string& instanceId) {
        if (service_) service_->removeWidget(instanceId);
    });
    service_ = std::make_shared<WidgetService>(registry, host);

    if (context) {
        auto& container = context->getServiceContainer();
        container.registerService<IWidgetService>(service_);
    }

    state_ = rex::modules::ModuleState::Initialized;
    return true;
}

bool WidgetModule::start() {
    state_ = rex::modules::ModuleState::Running;
    return true;
}

void WidgetModule::stop() {
    state_ = rex::modules::ModuleState::Stopped;
}

void WidgetModule::shutdown() {
    if (service_) {
        auto instances = service_->getInstances();
        for (const auto& inst : instances) {
            service_->removeWidget(inst.id);
        }
    }
    service_.reset();
    state_ = rex::modules::ModuleState::Unloaded;
}

void WidgetModule::update(float deltaTime) {
    (void)deltaTime;
}

std::unique_ptr<rex::modules::IModule> createWidgetModule() {
    return std::make_unique<WidgetModule>();
}

} // namespace rex::widgets
