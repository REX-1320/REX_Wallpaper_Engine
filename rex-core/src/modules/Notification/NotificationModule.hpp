#pragma once
#include "modules/interfaces/IModule.hpp"
#include "modules/ModuleMetadata.hpp"
#include "modules/ModuleState.hpp"
#include "services/NotificationService.hpp"
#include <memory>

namespace rex::modules { class IModuleContext; class EventBus; }

namespace rex::notifications {

class NotificationModule : public rex::modules::IModule {
public:
    NotificationModule();

    bool initialize(rex::modules::IModuleContext* context) override;
    bool start() override;
    void stop() override;
    void shutdown() override;

    const rex::modules::ModuleMetadata& getMetadata() const override { return metadata_; }
    rex::modules::ModuleState getState() const override { return state_; }

    void update(float deltaTime) override;

private:
    rex::modules::ModuleMetadata metadata_;
    rex::modules::ModuleState state_ = rex::modules::ModuleState::Unknown;

    std::unique_ptr<NotificationService> service_;
    rex::modules::EventBus* bus_ = nullptr;
    std::shared_ptr<rex::settings::ISettingsService> settingsService_;
};

}

extern "C" __declspec(dllexport) rex::modules::IModule* createModule() {
    return new rex::notifications::NotificationModule();
}
