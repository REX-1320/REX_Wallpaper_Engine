#include "NotificationModule.hpp"
#include "interfaces/INotificationService.hpp"
#include "renderer/NotificationRenderer.hpp"
#include "modules/EventBus.hpp"
#include "modules/interfaces/IModuleContext.hpp"
#include "modules/Settings/interfaces/ISettingsService.hpp"

namespace rex::notifications {

NotificationModule::NotificationModule() {
    metadata_.id = "rex.notification";
    metadata_.name = "Notification Framework";
    metadata_.version = "1.0.0";
    metadata_.category = "Core";
    metadata_.dependencies = {"rex.core", "rex.settings"};
    metadata_.autoStart = true;
    metadata_.builtIn = true;
    metadata_.enabledByDefault = true;
}

bool NotificationModule::initialize(rex::modules::IModuleContext* context) {
    bus_ = context->getService<rex::modules::EventBus>().get();
    settingsService_ = context->getService<rex::settings::ISettingsService>();

    service_ = std::make_unique<NotificationService>(*bus_, settingsService_);

    auto renderer = std::make_unique<NotificationRenderer>();
    if (settingsService_) {
        auto pos = settingsService_->getValue("notifications.position");
        if (pos.has_value() && std::holds_alternative<int64_t>(*pos)) {
            renderer->setPosition(static_cast<int>(std::get<int64_t>(*pos)), 20, 20);
        }
    }
    service_->setRenderer(std::move(renderer));

    state_ = rex::modules::ModuleState::Initialized;
    return true;
}

bool NotificationModule::start() {
    state_ = rex::modules::ModuleState::Running;
    return true;
}

void NotificationModule::stop() {
    state_ = rex::modules::ModuleState::Stopped;
}

void NotificationModule::shutdown() {
    if (service_) {
        service_->dismissAll();
    }
    state_ = rex::modules::ModuleState::Unloaded;
}

void NotificationModule::update(float deltaTime) {
    if (service_) {
        service_->update(deltaTime);
    }
}

}
