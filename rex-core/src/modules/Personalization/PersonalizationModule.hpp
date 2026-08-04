#pragma once
#include "modules/interfaces/IModule.hpp"
#include "modules/ModuleMetadata.hpp"
#include <memory>

namespace rex::personalization {

class IPersonalizationService;
class PersonalizationService;
namespace platform { class ITaskbarAdapter; class IExplorerAdapter; class IStartMenuAdapter; }

class PersonalizationModule : public rex::modules::IModule {
public:
    PersonalizationModule();
    ~PersonalizationModule() override;

    bool initialize(rex::modules::IModuleContext* ctx) override;
    bool start() override;
    void stop() override;
    void shutdown() override;

    const rex::modules::ModuleMetadata& getMetadata() const override;
    rex::modules::ModuleState getState() const override;

    std::shared_ptr<IPersonalizationService> getService() const;

private:
    rex::modules::ModuleMetadata metadata_;
    rex::modules::ModuleState state_ = rex::modules::ModuleState::Unknown;

    std::shared_ptr<PersonalizationService> m_service;
};

std::unique_ptr<rex::modules::IModule> createPersonalizationModule();

} // namespace
