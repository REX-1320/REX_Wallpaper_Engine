#pragma once

#include "modules/interfaces/IModule.hpp"
#include "modules/ModuleMetadata.hpp"

#include <memory>

namespace rex::widgets {

class WidgetService;

class WidgetModule : public rex::modules::IModule {
public:
    WidgetModule();
    ~WidgetModule() override = default;

    bool initialize(rex::modules::IModuleContext* context) override;
    bool start() override;
    void stop() override;
    void shutdown() override;

    void update(float deltaTime) override;

    const rex::modules::ModuleMetadata& getMetadata() const override { return metadata_; }
    rex::modules::ModuleState getState() const override { return state_; }

private:
    rex::modules::ModuleMetadata metadata_;
    rex::modules::ModuleState state_ = rex::modules::ModuleState::Unknown;

    std::shared_ptr<WidgetService> service_;
};

} // namespace rex::widgets
