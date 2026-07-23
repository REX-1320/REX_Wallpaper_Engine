#include "UIModule.hpp"
#include "modules/ServiceContainer.hpp"
#include "modules/EventBus.hpp"
#include "modules/interfaces/IModuleContext.hpp"

namespace rex::ui {

UIModule::UIModule() {
    metadata_.id = "rex.ui";
    metadata_.name = "UI Framework";
    metadata_.version = "1.0.0";
    metadata_.category = "Core";
    metadata_.dependencies = {"rex.core", "rex.settings"};
    metadata_.autoStart = true;
    metadata_.builtIn = true;
    metadata_.enabledByDefault = true;
}

bool UIModule::initialize(rex::modules::IModuleContext* context) {
    auto& container = context->getServiceContainer();
    bus_ = container.getService<rex::modules::EventBus>().get();

    themeRegistry_ = std::make_unique<ThemeRegistry>();
    tokenManager_ = std::make_unique<TokenManager>();
    styleResolver_ = std::make_unique<StyleResolver>(*tokenManager_);
    componentRegistry_ = std::make_unique<ComponentRegistry>();

    service_ = std::make_unique<ThemeService>(
        std::move(themeRegistry_),
        std::move(tokenManager_),
        std::move(styleResolver_),
        std::move(componentRegistry_));

    container.registerService<IThemeService>(std::shared_ptr<IThemeService>(service_.get(), [](auto*){}));
    state_ = rex::modules::ModuleState::Initialized;
    return true;
}

bool UIModule::start() {
    service_->loadBuiltInThemes();

    ComponentStyleDefinition buttonDef;
    buttonDef.componentType = "Button";
    buttonDef.defaultProperties = {
        {"background", "Color.Primary"},
        {"foreground", "Text.Primary"},
        {"borderRadius", "Radius.Medium"},
        {"padding", "Spacing.Medium"}
    };
    buttonDef.variants.push_back({"Primary", {
        {"background", "Color.Primary"},
        {"foreground", "Text.OnPrimary"}
    }});
    buttonDef.variants.push_back({"Danger", {
        {"background", "Color.Error"},
        {"foreground", "Text.OnError"}
    }});
    service_->registerComponent(buttonDef);

    state_ = rex::modules::ModuleState::Running;
    return true;
}

void UIModule::stop() {
    state_ = rex::modules::ModuleState::Stopped;
}

void UIModule::shutdown() {
    state_ = rex::modules::ModuleState::Unloaded;
}

}
