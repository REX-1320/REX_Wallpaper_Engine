#pragma once
#include "PageRenderer.h"
#include "Tabs.h"
#include "Table.h"
#include "PropertyGrid.h"
#include "Button.h"
#include "modules/EventBus.hpp"
#include "modules/Command/interfaces/ICommandService.hpp"
#include "modules/Search/interfaces/ISearchService.hpp"
#include "modules/Notification/interfaces/INotificationService.hpp"
#include "modules/Settings/interfaces/ISettingsService.hpp"
#include <memory>
#include <vector>
#include <deque>
#include <string>

class DeveloperPage : public PageRenderer {
public:
    DeveloperPage(ID2D1RenderTarget* rt, IDWriteFactory* dwrite,
                  const GlowStyle& style,
                  std::shared_ptr<rex::modules::EventBus> eventBus,
                  std::shared_ptr<rex::command::ICommandService> commandService,
                  std::shared_ptr<rex::search::ISearchService> searchService,
                  std::shared_ptr<rex::notifications::INotificationService> notificationService,
                  std::shared_ptr<rex::settings::ISettingsService> settingsService,
                  std::function<std::vector<std::string>()> getModuleList,
                  std::function<std::string(const std::string&)> getModuleState);
    void SetBounds(float x, float y, float w, float h);
    void Update(float dt) override;
    void Render(ID2D1RenderTarget* rt) override;
    void HandleClick(float mx, float my) override;

private:
    std::shared_ptr<rex::modules::EventBus> m_eventBus;
    std::shared_ptr<rex::command::ICommandService> m_commandService;
    std::shared_ptr<rex::search::ISearchService> m_searchService;
    std::shared_ptr<rex::notifications::INotificationService> m_notificationService;
    std::shared_ptr<rex::settings::ISettingsService> m_settingsService;
    std::function<std::vector<std::string>()> m_getModuleList;
    std::function<std::string(const std::string&)> m_getModuleState;

    Tabs m_tabs;
    Table m_eventTable;
    Table m_commandTable;
    Table m_moduleTable;
    PropertyGrid m_detailPanel;
    Button m_clearBtn;
    Button m_executeBtn;

    std::deque<std::vector<std::wstring>> m_eventRows;
    std::vector<std::vector<std::wstring>> m_commandRows;
    std::vector<std::vector<std::wstring>> m_moduleRows;
    int m_activeTab = 0;
    float m_x=0, m_y=0, m_w=0, m_h=0;

    std::vector<std::wstring> m_tabLabels;

    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_bodyFormat;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_textBrush;

    void RefreshModules();
    void RefreshCommands();
    void OnEvent(const rex::modules::ModuleEvent& event);
    void ClearEvents();
};
