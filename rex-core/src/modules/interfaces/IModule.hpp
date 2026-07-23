#pragma once

#include <string>
#include <memory>
#include "modules/ModuleMetadata.hpp"
#include "modules/ModuleState.hpp"
#include "modules/ModuleHealth.hpp"

namespace rex::modules {

class IModuleContext;

class IModule {
public:
    virtual ~IModule() = default;

    virtual bool initialize(IModuleContext* context) = 0;
    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual void shutdown() = 0;

    virtual bool pause()  { return false; }
    virtual bool resume() { return false; }

    virtual void enable()  {}
    virtual void disable() {}

    virtual void update(float deltaTime) {}

    virtual const ModuleMetadata& getMetadata() const = 0;
    virtual ModuleState getState() const = 0;

    virtual ModuleHealth getHealth() const { return ModuleHealth{}; }
};

} // namespace rex::modules
