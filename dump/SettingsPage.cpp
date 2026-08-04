#include "SettingsPage.h"
#include <sstream>
#include <algorithm>

SettingsPage::SettingsPage(ID2D1RenderTarget* rt, IDWriteFactory* dwrite,
                           const GlowStyle& style,
                           std::shared_ptr<rex::settings::ISettingsService> settingsService)
    : PageRenderer(rt, dwrite, style),
      m_settings(settingsService),
      m_toolbar(rt, dwrite, style),
      m_searchBar(rt, dwrite, style),
      m_propertyGrid(rt, dwrite, style)
{
    m_toolbar.SetActions({
        {L"Reset", L"\u21BA", [this]() { ResetCategory(); }},
        {L"Search", L"\u2609", [this]() { m_searchBar.SetFocused(true); }}
    });

    m_searchBar.SetOnSearch([this](const std::wstring& query) {
        if (m_activeCategoryIndex >= 0 && m_activeCategoryIndex < (int)m_categoryNames.size()) {
            LoadSettingsForCategory(m_activeCategoryIndex);
            std::string q(query.begin(), query.end());
            std::transform(q.begin(), q.end(), q.begin(), ::tolower);
            m_currentSettings.erase(
                std::remove_if(m_currentSettings.begin(), m_currentSettings.end(),
                    [&](const rex::settings::Setting& s) {
                        std::string nameLower = s.name;
                        std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
                        return nameLower.find(q) == std::string::npos &&
                               s.id.find(q) == std::string::npos;
                    }),
                m_currentSettings.end());
            RefreshPropertyGrid();
        }
    });

    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-us", &m_bodyFormat);
    rt->CreateSolidColorBrush(style.textColor, &m_textBrush);
}

void SettingsPage::SetBounds(float x, float y, float w, float h) {
    m_x = x; m_y = y; m_w = w; m_h = h;
    if (!m_initialized) {
        LoadCategories();
        m_initialized = true;
    }
}

void SettingsPage::Update(float dt) {
    m_toolbar.Update(dt);
    m_searchBar.Update(dt);
    for (auto& btn : m_categoryButtons) btn.Update(dt);
}

void SettingsPage::Render(ID2D1RenderTarget* rt) {
    if (!rt || !m_settings) return;

    float tbY = m_y;
    m_toolbar.SetBounds(m_x, tbY, m_w);
    m_toolbar.Render(rt);
    float contentY = tbY + 40;

    m_searchBar.SetPosition(m_x + 180, contentY, 400);
    m_searchBar.Render(rt);
    contentY += 40;

    float sidebarX = m_x;
    SectionHeader catHeader(L"Categories", rt, m_dwrite, m_style);
    catHeader.SetPosition(sidebarX + 10, contentY);
    catHeader.Render(rt);

    float catY = contentY + 30;
    for (size_t i = 0; i < m_categoryButtons.size(); ++i) {
        m_categoryButtons[i].SetPosition(sidebarX + 10, catY);
        m_categoryButtons[i].SetSelected(i == m_activeCategoryIndex);
        m_categoryButtons[i].Render(rt);
        catY += 28;
    }

    float wsX = sidebarX + 190;
    float wsW = m_w - 210;
    float wsY = contentY;

    if (m_activeCategoryIndex >= 0 && m_activeCategoryIndex < (int)m_categoryNames.size()) {
        std::wstring wName(m_categoryNames[m_activeCategoryIndex].begin(),
                           m_categoryNames[m_activeCategoryIndex].end());
        SectionHeader sh(wName, rt, m_dwrite, m_style);
        sh.SetPosition(wsX, wsY);
        sh.Render(rt);
        wsY += 30;

        m_propertyGrid.SetBounds(wsX, wsY, wsW);
        m_propertyGrid.Render(rt);
    } else {
        std::wstring hint = L"Select a category from the sidebar.";
        rt->DrawTextW(hint.c_str(), (UINT32)hint.size(), m_bodyFormat.Get(),
            D2D1::RectF(wsX, wsY, wsX + wsW, wsY + 30), m_textBrush.Get());
    }
}

void SettingsPage::HandleClick(float mx, float my) {
    if (m_toolbar.HitTest(mx, my)) return;
    if (m_searchBar.HitTest(mx, my)) { m_searchBar.SetFocused(true); return; }
    for (size_t i = 0; i < m_categoryButtons.size(); ++i) {
        if (m_categoryButtons[i].HitTest(mx, my)) {
            auto cb = m_categoryButtons[i].GetOnClick();
            if (cb) cb();
            return;
        }
    }
}

void SettingsPage::HandleChar(wchar_t ch) {
    m_searchBar.HandleChar(ch);
}

void SettingsPage::HandleKeyDown(int key) {
    m_searchBar.HandleKeyDown(key);
}

void SettingsPage::LoadCategories() {
    m_categoryButtons.clear();
    if (!m_settings) return;
    auto categories = m_settings->getAllCategories();
    m_categoryNames.clear();
    for (auto& cat : categories) {
        m_categoryNames.push_back(cat.id);
    }
    for (size_t i = 0; i < m_categoryNames.size(); ++i) {
        GlowTextButton btn(
            std::wstring(m_categoryNames[i].begin(), m_categoryNames[i].end()),
            m_rt, m_dwrite, m_style);
        btn.SetFontSize(14.0f);
        btn.SetOnClick([this, i]() {
            m_activeCategoryIndex = (int)i;
            LoadSettingsForCategory(i);
        });
        m_categoryButtons.push_back(std::move(btn));
    }
    if (!m_categoryButtons.empty()) {
        m_activeCategoryIndex = 0;
        LoadSettingsForCategory(0);
    }
}

void SettingsPage::LoadSettingsForCategory(int index) {
    if (!m_settings || index < 0 || index >= (int)m_categoryNames.size()) return;
    std::string catId = m_categoryNames[index];
    auto all = m_settings->getAllSettings();
    m_currentSettings.clear();
    for (auto& s : all) {
        if (s.category == catId) m_currentSettings.push_back(s);
    }
    RefreshPropertyGrid();
}

void SettingsPage::RefreshPropertyGrid() {
    m_propertyRows.clear();
    for (auto& s : m_currentSettings) {
        PropertyRow row;
        row.label = std::wstring(s.name.begin(), s.name.end());
        std::string valStr;
        auto& val = s.currentValue;
        if (std::holds_alternative<bool>(val)) valStr = std::get<bool>(val) ? "true" : "false";
        else if (std::holds_alternative<int64_t>(val)) valStr = std::to_string(std::get<int64_t>(val));
        else if (std::holds_alternative<double>(val)) valStr = std::to_string(std::get<double>(val));
        else if (std::holds_alternative<std::string>(val)) valStr = std::get<std::string>(val);
        row.value = std::wstring(valStr.begin(), valStr.end());
        row.editable = !s.readOnly;
        m_propertyRows.push_back(row);
    }
    m_propertyGrid.SetProperties(m_propertyRows);
}

void SettingsPage::ResetCategory() {
    if (!m_settings || m_activeCategoryIndex < 0) return;
    std::string catId = m_categoryNames[m_activeCategoryIndex];
    m_settings->resetCategory(catId);
    LoadSettingsForCategory(m_activeCategoryIndex);
}
