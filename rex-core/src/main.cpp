#include <windows.h>
#include "modules/ModuleOrchestrator.hpp"
#include "modules/EventBus.hpp"

// Module includes
#include "modules/Settings/SettingsModule.hpp"
#include "modules/Notification/NotificationModule.hpp"
#include "modules/Command/CommandModule.hpp"
#include "modules/Search/SearchModule.hpp"
#include "modules/Widgets/WidgetModule.hpp"
#include "modules/Personalization/PersonalizationModule.hpp"

using rex::modules::IModule;

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Create the orchestrator and register all modules
    rex::modules::ModuleOrchestrator orchestrator;

    // Core services
    orchestrator.registerBuiltIn("rex.settings", []() -> std::unique_ptr<IModule> {
        return std::make_unique<rex::settings::SettingsModule>();
    });
    orchestrator.registerBuiltIn("rex.notification", []() -> std::unique_ptr<IModule> {
        return std::make_unique<rex::notifications::NotificationModule>();
    });
    orchestrator.registerBuiltIn("rex.command", []() -> std::unique_ptr<IModule> {
        return std::make_unique<rex::command::CommandModule>();
    });
    orchestrator.registerBuiltIn("rex.search", []() -> std::unique_ptr<IModule> {
        return std::make_unique<rex::search::SearchModule>();
    });

    // Feature modules
    orchestrator.registerBuiltIn("rex.widgets", []() -> std::unique_ptr<IModule> {
        return std::make_unique<rex::widgets::WidgetModule>();
    });
    orchestrator.registerBuiltIn("rex.personalization", []() -> std::unique_ptr<IModule> {
        return std::make_unique<rex::personalization::PersonalizationModule>();
    });

    // Initialize all modules
    orchestrator.initialize();

    // No shell UI - keep the module host alive until the process exits
    MSG msg = {};
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    orchestrator.shutdown();
    return 0;
}
