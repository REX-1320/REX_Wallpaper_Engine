#include "PersonalizationService.hpp"

namespace rex::personalization {

PersonalizationService::PersonalizationService(
    std::unique_ptr<platform::ITaskbarAdapter> taskbar,
    std::unique_ptr<platform::IExplorerAdapter> explorer,
    std::unique_ptr<platform::IStartMenuAdapter> startMenu)
    : m_taskbar(std::move(taskbar))
    , m_explorer(std::move(explorer))
    , m_startMenu(std::move(startMenu)) {}

TaskbarSettings PersonalizationService::getTaskbarSettings() const {
    return m_taskbar ? m_taskbar->read() : TaskbarSettings{};
}

void PersonalizationService::setTaskbarSettings(const TaskbarSettings& s) {
    if (m_taskbar) m_taskbar->apply(s);
}

ExplorerSettings PersonalizationService::getExplorerSettings() const {
    return m_explorer ? m_explorer->read() : ExplorerSettings{};
}

void PersonalizationService::setExplorerSettings(const ExplorerSettings& s) {
    if (m_explorer) m_explorer->apply(s);
}

StartMenuSettings PersonalizationService::getStartMenuSettings() const {
    return m_startMenu ? m_startMenu->read() : StartMenuSettings{};
}

void PersonalizationService::setStartMenuSettings(const StartMenuSettings& s) {
    if (m_startMenu) m_startMenu->apply(s);
}

void PersonalizationService::applyAll() {
    if (m_taskbar) m_taskbar->apply(m_taskbar->read());
    if (m_explorer) m_explorer->apply(m_explorer->read());
    if (m_startMenu) m_startMenu->apply(m_startMenu->read());
}

void PersonalizationService::restoreDefaults() {
    if (m_taskbar) m_taskbar->apply(TaskbarSettings{});
    if (m_explorer) m_explorer->apply(ExplorerSettings{});
    if (m_startMenu) m_startMenu->apply(StartMenuSettings{});
}

} // namespace
