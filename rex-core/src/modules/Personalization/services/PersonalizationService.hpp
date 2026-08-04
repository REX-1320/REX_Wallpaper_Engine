#pragma once
#include "../interfaces/IPersonalizationService.hpp"
#include "../platform/ITaskbarAdapter.hpp"
#include "../platform/IExplorerAdapter.hpp"
#include "../platform/IStartMenuAdapter.hpp"
#include <memory>

namespace rex::personalization {

class PersonalizationService : public IPersonalizationService {
public:
    PersonalizationService(
        std::unique_ptr<platform::ITaskbarAdapter> taskbar,
        std::unique_ptr<platform::IExplorerAdapter> explorer,
        std::unique_ptr<platform::IStartMenuAdapter> startMenu);

    TaskbarSettings getTaskbarSettings() const override;
    void setTaskbarSettings(const TaskbarSettings& s) override;

    ExplorerSettings getExplorerSettings() const override;
    void setExplorerSettings(const ExplorerSettings& s) override;

    StartMenuSettings getStartMenuSettings() const override;
    void setStartMenuSettings(const StartMenuSettings& s) override;

    void applyAll() override;
    void restoreDefaults() override;

private:
    std::unique_ptr<platform::ITaskbarAdapter> m_taskbar;
    std::unique_ptr<platform::IExplorerAdapter> m_explorer;
    std::unique_ptr<platform::IStartMenuAdapter> m_startMenu;
};

} // namespace
