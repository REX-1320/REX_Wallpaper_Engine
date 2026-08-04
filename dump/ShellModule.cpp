#include "ShellModule.hpp"
#include "TransparentShellWindow.hpp"
#include "modules/ServiceContainer.hpp"
#include "modules/interfaces/IModuleContext.hpp"
#include "modules/UI/interfaces/IThemeService.hpp"
#include "modules/Search/interfaces/ISearchService.hpp"
#include "modules/Command/interfaces/ICommandService.hpp"
#include "modules/Notification/interfaces/INotificationService.hpp"
#include "modules/Settings/interfaces/ISettingsService.hpp"
#include "modules/Widgets/interfaces/IWidgetService.hpp"
#include "modules/Personalization/interfaces/IPersonalizationService.hpp"
#include "rex/wallpaper/IWallpaperEngine.hpp"
#include "modules/ConfigurationStore.hpp"
#include "modules/EventBus.hpp"
#include <cstring>
#include <windows.h>

namespace rex::shell {

ShellModule::ShellModule() {
    metadata_.id = "rex.shell";
    metadata_.name = "REX Shell";
    metadata_.version = "1.0.0";
    metadata_.category = "Core";
    metadata_.dependencies = {"rex.core", "rex.ui", "rex.search",
                              "rex.command", "rex.notification"};
    metadata_.autoStart = true;
    metadata_.builtIn = true;
}

ShellModule::~ShellModule() {
    shutdown();
}

bool ShellModule::initialize(rex::modules::IModuleContext* ctx) {
    auto& container = ctx->getServiceContainer();

    auto safeGet = [&container]<typename T>(T* dummy [[maybe_unused]]) -> std::shared_ptr<T> {
        if (container.hasService<T>())
            return container.getService<T>();
        return nullptr;
    };

    m_window = std::make_unique<TransparentShellWindow>(
        safeGet((rex::ui::IThemeService*)nullptr),
        safeGet((rex::search::ISearchService*)nullptr),
        safeGet((rex::command::ICommandService*)nullptr),
        safeGet((rex::notifications::INotificationService*)nullptr),
        safeGet((rex::wallpaper::IWallpaperEngine*)nullptr),
        safeGet((rex::settings::ISettingsService*)nullptr),
        safeGet((rex::widgets::IWidgetService*)nullptr),
        safeGet((rex::personalization::IPersonalizationService*)nullptr),
        safeGet((rex::modules::ConfigurationStore*)nullptr),
        safeGet((rex::modules::EventBus*)nullptr));

    state_ = rex::modules::ModuleState::Initialized;
    return true;
}

bool ShellModule::start() {
    if (m_window) {
        m_windowThread = std::thread([this]() {
            m_window->Run();
        });
        m_windowThread.detach();
    }
    state_ = rex::modules::ModuleState::Running;
    return true;
}

void ShellModule::stop() {
    if (m_window) m_window->RequestClose();
    state_ = rex::modules::ModuleState::Stopped;
}

void ShellModule::shutdown() {
    stop();
    m_window.reset();
    state_ = rex::modules::ModuleState::Unloaded;
}

} // namespace
