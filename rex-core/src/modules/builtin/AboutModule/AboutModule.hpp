#pragma once

#include "modules/interfaces/IModule.hpp"
#include "modules/ModuleMetadata.hpp"

namespace rex::modules {

class AboutModule : public IModule {
public:
    AboutModule();
    ~AboutModule() override = default;

    bool initialize(IModuleContext* context) override;
    bool start() override;
    void stop() override;
    void shutdown() override;

    bool pause() override { return false; }
    bool resume() override { return false; }

    void update(float deltaTime) override;

    const ModuleMetadata& getMetadata() const override;
    ModuleState getState() const override;

private:
    ModuleMetadata metadata_;
    ModuleState state_ = ModuleState::Unknown;
};

} // namespace rex::modules
