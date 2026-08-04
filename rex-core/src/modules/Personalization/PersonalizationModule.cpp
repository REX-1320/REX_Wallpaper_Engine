#include "PersonalizationModule.hpp"
#include "modules/interfaces/IModuleContext.hpp"
#include "modules/ServiceContainer.hpp"
#include "interfaces/IPersonalizationService.hpp"
#include "services/PersonalizationService.hpp"
#include "platform/ITaskbarAdapter.hpp"
#include "platform/IExplorerAdapter.hpp"
#include "platform/IStartMenuAdapter.hpp"

namespace rex::personalization {

PersonalizationModule::PersonalizationModule() {
    metadata_.id = "rex.personalization";
    metadata_.name = "Personalization";
    metadata_.version = "1.0.0";
    metadata_.category = "Core";
    metadata_.dependencies = {"rex.core"};
    metadata_.autoStart = true;
    metadata_.builtIn = true;
    metadata_.enabledByDefault = true;
}

PersonalizationModule::~PersonalizationModule() = default;

bool PersonalizationModule::initialize(rex::modules::IModuleContext* ctx) {
    auto taskbar = std::make_unique<platform::WinTaskbarAdapter>();
    auto explorer = std::make_unique<platform::WinExplorerAdapter>();
    auto startMenu = std::make_unique<platform::WinStartMenuAdapter>();

    m_service = std::make_shared<PersonalizationService>(
        std::move(taskbar), std::move(explorer), std::move(startMenu));

    ctx->getServiceContainer().registerService<IPersonalizationService>(m_service);

    state_ = rex::modules::ModuleState::Initialized;
    return true;
}

bool PersonalizationModule::start() {
    state_ = rex::modules::ModuleState::Running;
    return true;
}

void PersonalizationModule::stop() {
    state_ = rex::modules::ModuleState::Stopped;
}

void PersonalizationModule::shutdown() {
    m_service.reset();
    state_ = rex::modules::ModuleState::Unloaded;
}

const rex::modules::ModuleMetadata& PersonalizationModule::getMetadata() const {
    return metadata_;
}

rex::modules::ModuleState PersonalizationModule::getState() const {
    return state_;
}

std::shared_ptr<IPersonalizationService> PersonalizationModule::getService() const {
    return std::static_pointer_cast<IPersonalizationService>(m_service);
}

std::unique_ptr<rex::modules::IModule> createPersonalizationModule() {
    return std::make_unique<PersonalizationModule>();
}

} // namespace
