#pragma once
#include "modules/interfaces/IModule.hpp"
#include <memory>
#include <thread>

namespace rex::shell {

class TransparentShellWindow;

class ShellModule : public rex::modules::IModule {
public:
    ShellModule();
    ~ShellModule() override;

    bool initialize(rex::modules::IModuleContext* context) override;
    bool start() override;
    void stop() override;
    void shutdown() override;

    const rex::modules::ModuleMetadata& getMetadata() const override { return metadata_; }
    rex::modules::ModuleState getState() const override { return state_; }

private:
    rex::modules::ModuleMetadata metadata_;
    rex::modules::ModuleState state_ = rex::modules::ModuleState::Unknown;
    std::unique_ptr<TransparentShellWindow> m_window;
    std::thread m_windowThread;
};

} // namespace
