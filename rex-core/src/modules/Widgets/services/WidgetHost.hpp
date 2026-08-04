#pragma once

#include <windows.h>
#include <string>
#include <unordered_map>
#include <memory>
#include <functional>

#include "../models/WidgetInstance.hpp"
#include "../models/WidgetDefinition.hpp"

namespace rex::widgets {

class WidgetRegistry;

class WidgetHost {
public:
    explicit WidgetHost(std::shared_ptr<WidgetRegistry> registry);
    ~WidgetHost();

    bool createWindow(const WidgetInstance& instance, const WidgetDefinition& def);
    void destroyWindow(const std::string& instanceId);
    void moveWindow(const std::string& instanceId, int x, int y);
    void setVisible(const std::string& instanceId, bool visible);

    using CloseCallback = std::function<void(const std::string& instanceId)>;
    void setOnWidgetClose(CloseCallback cb) { m_onWidgetClose = std::move(cb); }

private:
    struct WidgetWindow {
        HWND hwnd = nullptr;
        WidgetInstance instance;
    };

    std::unordered_map<std::string, WidgetWindow> m_windows;
    std::shared_ptr<WidgetRegistry> m_registry;
    CloseCallback m_onWidgetClose;

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static ATOM RegisterWidgetClass();
    static ATOM s_widgetClassAtom;
};

} // namespace rex::widgets
