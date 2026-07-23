#pragma once
#include "modules/interfaces/IModule.hpp"
#include "modules/ModuleMetadata.hpp"
#include "modules/ModuleState.hpp"
#include "services/ThemeService.hpp"
#include <memory>

namespace rex::modules { class IModuleContext; class EventBus; }

namespace rex::ui {

class UIModule : public rex::modules::IModule {
public:
    UIModule();

    bool initialize(rex::modules::IModuleContext* context) override;
    bool start() override;
    void stop() override;
    void shutdown() override;

    const rex::modules::ModuleMetadata& getMetadata() const override { return metadata_; }
    rex::modules::ModuleState getState() const override { return state_; }

private:
    rex::modules::ModuleMetadata metadata_;
    rex::modules::ModuleState state_ = rex::modules::ModuleState::Unknown;

    std::unique_ptr<ThemeRegistry> themeRegistry_;
    std::unique_ptr<TokenManager> tokenManager_;
    std::unique_ptr<StyleResolver> styleResolver_;
    std::unique_ptr<ComponentRegistry> componentRegistry_;
    std::unique_ptr<ThemeService> service_;

    rex::modules::EventBus* bus_ = nullptr;
};

}
