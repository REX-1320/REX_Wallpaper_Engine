#include "DeveloperPage.h"
#include <sstream>
#include <iomanip>
#include <chrono>

using namespace std::chrono;

DeveloperPage::DeveloperPage(ID2D1RenderTarget* rt, IDWriteFactory* dwrite,
                             const GlowStyle& style,
                             std::shared_ptr<rex::modules::EventBus> eventBus,
                             std::shared_ptr<rex::command::ICommandService> commandService,
                             std::shared_ptr<rex::search::ISearchService> searchService,
                             std::shared_ptr<rex::notifications::INotificationService> notificationService,
                             std::shared_ptr<rex::settings::ISettingsService> settingsService,
                             std::function<std::vector<std::string>()> getModuleList,
                             std::function<std::string(const std::string&)> getModuleState)
    : PageRenderer(rt, dwrite, style),
      m_eventBus(eventBus),
      m_commandService(commandService),
      m_searchService(searchService),
      m_notificationService(notificationService),
      m_settingsService(settingsService),
      m_getModuleList(getModuleList),
      m_getModuleState(getModuleState),
      m_tabs(rt, dwrite, style),
      m_eventTable(rt, dwrite, style),
      m_commandTable(rt, dwrite, style),
      m_moduleTable(rt, dwrite, style),
      m_detailPanel(rt, dwrite, style),
      m_clearBtn(L"Clear", rt, dwrite, style),
      m_executeBtn(L"Execute", rt, dwrite, style)
{
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-us", &m_bodyFormat);
    rt->CreateSolidColorBrush(style.textColor, &m_textBrush);

    m_tabLabels = {L"Events", L"Commands", L"Modules", L"Search", L"Notifications", L"Settings", L"Perf"};
    m_tabs.SetTabs(m_tabLabels);
    m_tabs.SetOnChange([this](int idx) { m_activeTab = idx; });

    if (m_eventBus) {
        m_eventBus->subscribe([this](const rex::modules::ModuleEvent& ev) {
            OnEvent(ev);
        });
    }

    m_eventTable.SetColumns({{L"Time", 80}, {L"Module", 120}, {L"Event", 100}, {L"Details", 400}});
    m_commandTable.SetColumns({{L"ID", 200}, {L"Name", 150}, {L"Category", 100}, {L"Enabled", 80}});
    m_moduleTable.SetColumns({{L"Module", 150}, {L"Status", 100}, {L"Version", 100}, {L"Category", 150}});

    m_clearBtn.SetOnClick([this]() { ClearEvents(); });
    m_executeBtn.SetOnClick([this]() { RefreshCommands(); });

    RefreshModules();
    RefreshCommands();
}

void DeveloperPage::SetBounds(float x, float y, float w, float h) {
    m_x = x; m_y = y; m_w = w; m_h = h;
}

void DeveloperPage::Update(float dt) {
    m_tabs.Update(dt);
    m_clearBtn.Update(dt);
    m_executeBtn.Update(dt);
}

void DeveloperPage::Render(ID2D1RenderTarget* rt) {
    if (!rt) return;
    float x = m_x, y = m_y, w = m_w;
    float tabH = 40;

    m_tabs.SetBounds(x, y, w);
    m_tabs.Render(rt);
    y += tabH;

    float contentH = m_h - tabH;

    if (m_activeTab == 0) {
        std::vector<TableRow> rows;
        for (auto& ev : m_eventRows) {
            TableRow row;
            row.cells = ev;
            rows.push_back(row);
        }
        m_eventTable.SetRows(rows);
        m_eventTable.SetBounds(x, y, w, contentH - 36);
        m_eventTable.Render(rt);
        m_clearBtn.SetPosition(x + w - 80, y + contentH - 32);
        m_clearBtn.Render(rt);
    } else if (m_activeTab == 1) {
        RefreshCommands();
        std::vector<TableRow> rows;
        for (auto& cmd : m_commandRows) {
            TableRow row;
            row.cells = cmd;
            rows.push_back(row);
        }
        m_commandTable.SetRows(rows);
        m_commandTable.SetBounds(x, y, w, contentH - 36);
        m_commandTable.Render(rt);
        m_executeBtn.SetPosition(x + w - 80, y + contentH - 32);
        m_executeBtn.Render(rt);
    } else if (m_activeTab == 2) {
        RefreshModules();
        std::vector<TableRow> rows;
        for (auto& mod : m_moduleRows) {
            TableRow row;
            row.cells = mod;
            rows.push_back(row);
        }
        m_moduleTable.SetRows(rows);
        m_moduleTable.SetBounds(x, y, w, contentH);
        m_moduleTable.Render(rt);
    } else {
        std::wstring txt = L"Tab: " + m_tabLabels[m_activeTab];
        rt->DrawTextW(txt.c_str(), (UINT32)txt.size(), m_bodyFormat.Get(),
            D2D1::RectF(x, y, x+w, y+30), m_textBrush.Get());
    }
}

void DeveloperPage::HandleClick(float mx, float my) {
    m_tabs.HitTest(mx, my);
    m_clearBtn.HitTest(mx, my);
    m_executeBtn.HitTest(mx, my);
}

void DeveloperPage::OnEvent(const rex::modules::ModuleEvent& event) {
    auto now = system_clock::now();
    auto time = system_clock::to_time_t(now);
    std::tm tm;
    localtime_s(&tm, &time);
    std::wostringstream timeStr;
    timeStr << std::put_time(&tm, L"%H:%M:%S");

    std::wstring typeStr;
    switch (event.type) {
        case rex::modules::ModuleEventType::ModuleLoaded: typeStr = L"Loaded"; break;
        case rex::modules::ModuleEventType::ModuleStarted: typeStr = L"Started"; break;
        case rex::modules::ModuleEventType::ModuleStopped: typeStr = L"Stopped"; break;
        case rex::modules::ModuleEventType::ModuleFailed: typeStr = L"Failed"; break;
        case rex::modules::ModuleEventType::ThemeChanged: typeStr = L"ThemeChanged"; break;
        case rex::modules::ModuleEventType::ProfileSaved: typeStr = L"ProfileSaved"; break;
        case rex::modules::ModuleEventType::ProfileDeleted: typeStr = L"ProfileDeleted"; break;
        case rex::modules::ModuleEventType::ProfileLoaded: typeStr = L"ProfileLoaded"; break;
        case rex::modules::ModuleEventType::SettingChanged: typeStr = L"SettingChanged"; break;
        default: typeStr = L"Other"; break;
    }

    m_eventRows.push_front({
        timeStr.str(),
        std::wstring(event.moduleId.begin(), event.moduleId.end()),
        typeStr,
        L""
    });
    if (m_eventRows.size() > 200) m_eventRows.pop_back();
}

void DeveloperPage::ClearEvents() {
    m_eventRows.clear();
}

void DeveloperPage::RefreshModules() {
    m_moduleRows.clear();
    auto modules = m_getModuleList();
    for (auto& mod : modules) {
        std::wstring wmod(mod.begin(), mod.end());
        auto stateStr = m_getModuleState(mod);
        std::wstring state(stateStr.begin(), stateStr.end());
        m_moduleRows.push_back({wmod, state, L"1.0", L""});
    }
}

void DeveloperPage::RefreshCommands() {
    m_commandRows.clear();
    if (!m_commandService) return;
    auto cmds = m_commandService->getAllCommands();
    for (auto& cmd : cmds) {
        std::wstring id(cmd.id.begin(), cmd.id.end());
        std::wstring name;
        std::wstring cat;
        if (cmd.definition) {
            name = std::wstring(cmd.definition->name.begin(), cmd.definition->name.end());
            cat = std::wstring(cmd.definition->category.begin(), cmd.definition->category.end());
        }
        std::wstring enabled = cmd.enabled ? L"Yes" : L"No";
        m_commandRows.push_back({id, name, cat, enabled});
    }
}
