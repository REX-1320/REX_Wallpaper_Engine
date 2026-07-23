#include "AboutModule.hpp"
#include <iostream>

namespace rex::modules {

AboutModule::AboutModule() {
    metadata_.id          = "builtin.about";
    metadata_.name        = "About";
    metadata_.version     = "1.0.0";
    metadata_.author      = "REX Team";
    metadata_.description = "Displays system and application information";
    metadata_.category    = "System";
    metadata_.builtIn     = true;
    metadata_.autoStart   = true;
    metadata_.enabledByDefault = true;
}

bool AboutModule::initialize(IModuleContext* context) {
    state_ = ModuleState::Initialized;
    return true;
}

bool AboutModule::start() {
    state_ = ModuleState::Running;
    return true;
}

void AboutModule::stop() {
    state_ = ModuleState::Stopped;
}

void AboutModule::shutdown() {
    state_ = ModuleState::Unloaded;
}

void AboutModule::update(float deltaTime) {
}

const ModuleMetadata& AboutModule::getMetadata() const {
    return metadata_;
}

ModuleState AboutModule::getState() const {
    return state_;
}

std::unique_ptr<IModule> createAboutModule() {
    return std::make_unique<AboutModule>();
}

} // namespace rex::modules
