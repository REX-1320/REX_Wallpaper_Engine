#include "HomePage.h"
#include <sstream>
#include <iomanip>
#include <ctime>

using namespace std::chrono;

HomePage::HomePage(ID2D1RenderTarget* rt, IDWriteFactory* dwrite,
                   const GlowStyle& style,
                   std::shared_ptr<rex::wallpaper::IWallpaperEngine> wallpaperEngine,
                   std::shared_ptr<rex::widgets::IWidgetService> widgetService,
                   std::shared_ptr<rex::personalization::IPersonalizationService> personalizationService,
                   std::shared_ptr<rex::settings::ISettingsService> settingsService,
                   std::shared_ptr<rex::notifications::INotificationService> notificationService)
    : PageRenderer(rt, dwrite, style),
      m_wallpaperEngine(wallpaperEngine),
      m_widgetService(widgetService),
      m_personalizationService(personalizationService),
      m_settingsService(settingsService),
      m_notificationService(notificationService),
      m_recentGallery(rt, dwrite, style, 5, 100, 100, 8),
      m_downloadProgress(rt, style)
{
    m_lastClockUpdate = steady_clock::now();

    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 28.0f, L"en-us", &m_titleFormat);
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"en-us", &m_bodyFormat);
    rt->CreateSolidColorBrush(style.textColor, &m_textBrush);

    struct Action { std::wstring label; std::function<void()> cb; };
    std::vector<Action> actions = {
        {L"Import",    []() {}},
        {L"Browse",    []() {}},
        {L"Theme",     []() {}},
        {L"Widgets",   []() {}},
        {L"Market",    []() {}},
        {L"Profile",   []() {}}
    };
    for (auto& a : actions) {
        Button btn(a.label, rt, dwrite, style);
        btn.SetOnClick(std::move(a.cb));
        btn.SetSize(90, 32);
        m_quickActions.push_back(std::move(btn));
    }
}

void HomePage::SetBounds(float x, float y, float w, float h) {
    m_x = x; m_y = y; m_w = w; m_h = h;
    RefreshData();
}

void HomePage::Update(float dt) {
    auto now = steady_clock::now();
    if (duration_cast<seconds>(now - m_lastClockUpdate).count() >= 1) {
        m_lastClockUpdate = now;
        time_t t = time(nullptr);
        struct tm tm;
        localtime_s(&tm, &t);
        wchar_t buf[10];
        wcsftime(buf, 10, L"%H:%M", &tm);
        m_currentTime = buf;
    }

    for (auto& btn : m_quickActions) btn.Update(dt);
    m_recentGallery.Update(dt);
    m_downloadProgress.Update(dt);

    static auto lastRefresh = steady_clock::now();
    if (duration_cast<seconds>(now - lastRefresh).count() >= 2) {
        RefreshData();
        lastRefresh = now;
    }
}

void HomePage::RefreshData() {
    if (m_wallpaperEngine) {
        auto scene = m_wallpaperEngine->GetCurrentScene();
        m_wallpaperName = std::wstring(scene.name.begin(), scene.name.end());
        m_wallpaperType = std::wstring(scene.type.begin(), scene.type.end());
        m_wallpaperPlaying = m_wallpaperEngine->IsPlaying();
        m_wallpaperResolution = L"";
    }
    if (m_widgetService) {
        m_runningWidgets = (int)m_widgetService->getInstances().size();
    }

    m_cpuPercent = 18.0f; m_gpuPercent = 11.0f; m_ramPercent = 35.0f;
    m_storagePercent = 45.0f;

    m_downloadProgressValue = 0.0f;
    m_downloadLabel = L"No active downloads";

    m_activity = {L"Installed Glass Theme", L"Updated Widget", L"Imported Wallpaper"};

    m_recentItems.clear();
    for (int i = 1; i <= 5; ++i) {
        GalleryItem item;
        item.title = L"Wallpaper " + std::to_wstring(i);
        item.subtitle = L"Static";
        m_recentItems.push_back(item);
    }
}

void HomePage::Render(ID2D1RenderTarget* rt) {
    if (!rt) return;
    float x = m_x, y = m_y, w = m_w;

    SectionHeader header(m_greeting, rt, m_dwrite, m_style);
    header.SetPosition(x + 10, y);
    header.Render(rt);

    rt->DrawTextW(m_currentTime.c_str(), (UINT32)m_currentTime.size(), m_titleFormat.Get(),
        D2D1::RectF(x + w - 200, y, x + w - 10, y + 30), m_textBrush.Get());
    y += 40;

    Card activeCard(rt, m_dwrite, m_style);
    activeCard.SetBounds(x + 10, y, 300, 180);
    activeCard.SetTitle(L"Active Wallpaper");
    activeCard.SetSubtitle(m_wallpaperName + L"\n" + m_wallpaperType + L" \u2022 " + m_wallpaperResolution);
    activeCard.Render(rt);

    Card widgetsCard(rt, m_dwrite, m_style);
    widgetsCard.SetBounds(x + 320, y, 200, 180);
    widgetsCard.SetTitle(L"Running Widgets");
    widgetsCard.SetSubtitle(std::to_wstring(m_runningWidgets) + L" enabled");
    widgetsCard.Render(rt);

    Card themeCard(rt, m_dwrite, m_style);
    themeCard.SetBounds(x + 530, y, 200, 180);
    themeCard.SetTitle(L"Current Theme");
    themeCard.SetSubtitle(m_currentTheme);
    themeCard.Render(rt);

    y += 190;

    struct Stat { std::wstring name; float value; };
    std::vector<Stat> stats = {
        {L"CPU", m_cpuPercent}, {L"GPU", m_gpuPercent},
        {L"Memory", m_ramPercent}, {L"Storage", m_storagePercent}
    };
    float cardW = 150, cardH = 80;
    float sx = x + 10;
    for (auto& stat : stats) {
        Card statCard(rt, m_dwrite, m_style);
        statCard.SetBounds(sx, y, cardW, cardH);
        statCard.SetTitle(stat.name);
        statCard.SetSubtitle(std::to_wstring((int)stat.value) + L"%");
        statCard.Render(rt);
        sx += cardW + 10;
    }

    y += 90;

    SectionHeader qaHeader(L"Quick Actions", rt, m_dwrite, m_style);
    qaHeader.SetPosition(x + 10, y);
    qaHeader.Render(rt);
    y += 30;
    float qax = x + 10;
    for (auto& btn : m_quickActions) {
        btn.SetPosition(qax, y);
        btn.Render(rt);
        qax += 100;
    }

    y += 50;

    SectionHeader recentHeader(L"Recent Wallpapers", rt, m_dwrite, m_style);
    recentHeader.SetPosition(x + 10, y);
    recentHeader.Render(rt);
    y += 30;
    m_recentGallery.SetBounds(x + 10, y, w - 20, 100);
    m_recentGallery.SetItems(m_recentItems);
    m_recentGallery.Render(rt);

    y += 110;

    SectionHeader dlHeader(L"Downloads", rt, m_dwrite, m_style);
    dlHeader.SetPosition(x + 10, y);
    dlHeader.Render(rt);
    y += 25;
    m_downloadProgress.SetBounds(x + 10, y, 300, 6);
    m_downloadProgress.SetProgress(m_downloadProgressValue);
    m_downloadProgress.Render(rt);
    rt->DrawTextW(m_downloadLabel.c_str(), (UINT32)m_downloadLabel.size(), m_bodyFormat.Get(),
        D2D1::RectF(x + 10, y + 10, 300, 20), m_textBrush.Get());

    y += 30;

    SectionHeader actHeader(L"Recent Activity", rt, m_dwrite, m_style);
    actHeader.SetPosition(x + 10, y);
    actHeader.Render(rt);
    y += 25;
    for (auto& act : m_activity) {
        rt->DrawTextW(act.c_str(), (UINT32)act.size(), m_bodyFormat.Get(),
            D2D1::RectF(x + 10, y, 300, 20), m_textBrush.Get());
        y += 20;
    }
}

void HomePage::HandleClick(float mx, float my) {
    for (auto& btn : m_quickActions) {
        if (btn.HitTest(mx, my)) {
            auto cb = btn.GetOnClick();
            if (cb) cb();
            return;
        }
    }
}
