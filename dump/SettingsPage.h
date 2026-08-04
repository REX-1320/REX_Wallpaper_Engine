#pragma once
#include "PageRenderer.h"
#include "Toolbar.h"
#include "SearchBar.h"
#include "PropertyGrid.h"
#include "SectionHeader.h"
#include "Button.h"
#include "GlowTextButton.h"
#include "modules/Settings/interfaces/ISettingsService.hpp"
#include <memory>
#include <vector>
#include <string>

class SettingsPage : public PageRenderer {
public:
    SettingsPage(ID2D1RenderTarget* rt, IDWriteFactory* dwrite,
                 const GlowStyle& style,
                 std::shared_ptr<rex::settings::ISettingsService> settingsService);
    void SetBounds(float x, float y, float w, float h);
    void Update(float dt) override;
    void Render(ID2D1RenderTarget* rt) override;
    void HandleClick(float mx, float my) override;
    void HandleChar(wchar_t ch) override;
    void HandleKeyDown(int key) override;

private:
    std::shared_ptr<rex::settings::ISettingsService> m_settings;

    Toolbar m_toolbar;
    SearchBar m_searchBar;
    PropertyGrid m_propertyGrid;
    std::vector<GlowTextButton> m_categoryButtons;
    int m_activeCategoryIndex = -1;

    std::vector<std::string> m_categoryNames;
    std::vector<rex::settings::Setting> m_currentSettings;
    std::vector<PropertyRow> m_propertyRows;

    float m_x = 0, m_y = 0, m_w = 0, m_h = 0;
    bool m_initialized = false;

    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_bodyFormat;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_textBrush;

    void LoadCategories();
    void LoadSettingsForCategory(int index);
    void RefreshPropertyGrid();
    void ResetCategory();
};
