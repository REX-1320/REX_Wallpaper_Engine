#include "PerformancePage.h"
#include <windows.h>
#include <psapi.h>
#include <sstream>


PerformancePage::PerformancePage(ID2D1RenderTarget* rt, IDWriteFactory* dwrite,
                                 const GlowStyle& style,
                                 std::shared_ptr<rex::wallpaper::IWallpaperEngine> wallpaperEngine,
                                 std::shared_ptr<rex::widgets::IWidgetService> widgetService)
    : PageRenderer(rt, dwrite, style),
      m_wallpaperEngine(wallpaperEngine),
      m_widgetService(widgetService),
      m_processTable(rt, dwrite, style),
      m_pauseOnFullscreenToggle(rt, style),
      m_pauseOnBatteryToggle(rt, style)
{
    dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"en-us", &m_bodyFormat);
    rt->CreateSolidColorBrush(style.textColor, &m_textBrush);

    for (int i = 0; i < 6; ++i) {
        m_progressBars.emplace_back(rt, style);
    }

    m_statLabels = {L"CPU", L"GPU", L"RAM", L"VRAM", L"Storage", L"Network"};
    m_statValues.assign(6, 0.0f);

    m_processTable.SetColumns({{L"Module", 180}, {L"CPU %", 80}, {L"RAM MB", 100}, {L"GPU %", 80}});

    m_pauseOnFullscreenToggle.SetOnChange([this](bool checked) {
        m_pauseOnFullscreen = checked;
    });
    m_pauseOnBatteryToggle.SetOnChange([this](bool checked) {
        (void)checked;
    });

    m_lastRefresh = std::chrono::steady_clock::now();
    RefreshStats();
}

void PerformancePage::SetBounds(float x, float y, float w, float h) {
    m_x = x; m_y = y; m_w = w; m_h = h;
}

void PerformancePage::Update(float dt) {
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - m_lastRefresh).count() >= 1) {
        RefreshStats();
        m_lastRefresh = now;
    }
    for (auto& bar : m_progressBars) bar.Update(dt);
    m_pauseOnFullscreenToggle.Update(dt);
    m_pauseOnBatteryToggle.Update(dt);
    m_frameTime = dt;
    if (dt > 0.0f) m_fps = (int)(1.0f / dt);
}

void PerformancePage::Render(ID2D1RenderTarget* rt) {
    if (!rt) return;
    float x = m_x, y = m_y, w = m_w;

    SectionHeader header(L"System Performance", rt, m_dwrite, m_style);
    header.SetPosition(x + 10, y + 10);
    header.Render(rt);
    y += 40;

    std::wstring fpsStr = L"FPS: " + std::to_wstring(m_fps) +
        L"  Frame: " + std::to_wstring((int)(m_frameTime * 1000.0f)) + L" ms";
    rt->DrawTextW(fpsStr.c_str(), (UINT32)fpsStr.size(), m_bodyFormat.Get(),
        D2D1::RectF(x + 10, y, x + w, y + 20), m_textBrush.Get());
    y += 30;

    float barW = w - 220, barH = 24;
    for (size_t i = 0; i < m_statLabels.size(); ++i) {
        rt->DrawTextW(m_statLabels[i].c_str(), (UINT32)m_statLabels[i].size(), m_bodyFormat.Get(),
            D2D1::RectF(x + 10, y, x + 140, y + barH), m_textBrush.Get());
        m_progressBars[i].SetBounds(x + 150, y, barW, barH);
        m_progressBars[i].SetProgress(m_statValues[i] / 100.0f);
        m_progressBars[i].Render(rt);
        std::wstring valStr = std::to_wstring((int)m_statValues[i]) + L"%";
        rt->DrawTextW(valStr.c_str(), (UINT32)valStr.size(), m_bodyFormat.Get(),
            D2D1::RectF(x + 150 + barW + 8, y, x + w, y + barH), m_textBrush.Get());
        y += 30;
    }

    y += 10;

    SectionHeader procHeader(L"Module Usage", rt, m_dwrite, m_style);
    procHeader.SetPosition(x + 10, y);
    procHeader.Render(rt);
    y += 30;
    m_processTable.SetBounds(x + 10, y, w - 20, 200);
    m_processTable.Render(rt);
    y += 220;

    SectionHeader settingsHeader(L"Performance Settings", rt, m_dwrite, m_style);
    settingsHeader.SetPosition(x + 10, y);
    settingsHeader.Render(rt);
    y += 30;
    m_pauseOnFullscreenToggle.SetPosition(x + 20, y);
    m_pauseOnFullscreenToggle.Render(rt);
    rt->DrawTextW(L"Pause wallpaper when fullscreen app is running", 47, m_bodyFormat.Get(),
        D2D1::RectF(x + 70, y, x + w, y + 24), m_textBrush.Get());
    y += 30;
    m_pauseOnBatteryToggle.SetPosition(x + 20, y);
    m_pauseOnBatteryToggle.Render(rt);
    rt->DrawTextW(L"Pause wallpaper on battery", 26, m_bodyFormat.Get(),
        D2D1::RectF(x + 70, y, x + w, y + 24), m_textBrush.Get());
}

void PerformancePage::HandleClick(float mx, float my) {
    if (m_pauseOnFullscreenToggle.HitTest(mx, my))
        m_pauseOnFullscreenToggle.SetChecked(!m_pauseOnFullscreenToggle.IsChecked());
    if (m_pauseOnBatteryToggle.HitTest(mx, my))
        m_pauseOnBatteryToggle.SetChecked(!m_pauseOnBatteryToggle.IsChecked());
}

void PerformancePage::RefreshStats() {
    FILETIME idle, kernel, user;
    if (GetSystemTimes(&idle, &kernel, &user)) {
        static ULARGE_INTEGER prevIdle = {0}, prevKernel = {0}, prevUser = {0};
        ULARGE_INTEGER curIdle, curKernel, curUser;
        curIdle.LowPart = idle.dwLowDateTime; curIdle.HighPart = idle.dwHighDateTime;
        curKernel.LowPart = kernel.dwLowDateTime; curKernel.HighPart = kernel.dwHighDateTime;
        curUser.LowPart = user.dwLowDateTime; curUser.HighPart = user.dwHighDateTime;

        if (prevIdle.QuadPart != 0) {
            ULONGLONG idleDiff = curIdle.QuadPart - prevIdle.QuadPart;
            ULONGLONG kernelDiff = curKernel.QuadPart - prevKernel.QuadPart;
            ULONGLONG userDiff = curUser.QuadPart - prevUser.QuadPart;
            ULONGLONG totalDiff = kernelDiff + userDiff;
            if (totalDiff > 0)
                m_statValues[0] = (float)((double)(totalDiff - idleDiff) * 100.0 / totalDiff);
        }
        prevIdle = curIdle; prevKernel = curKernel; prevUser = curUser;
    }

    MEMORYSTATUSEX mem = {sizeof(MEMORYSTATUSEX)};
    if (GlobalMemoryStatusEx(&mem)) {
        m_statValues[2] = (float)mem.dwMemoryLoad;
    }

    m_statValues[1] = 11.0f;
    m_statValues[3] = 25.0f;
    m_statValues[4] = 45.0f;
    m_statValues[5] = 12.0f;

    std::vector<TableRow> rows;
    auto addRow = [&](const std::wstring& name, float cpu, float ram, float gpu) {
        TableRow r;
        r.cells = {name, std::to_wstring((int)cpu) + L"%",
                   std::to_wstring((int)ram) + L" MB", std::to_wstring((int)gpu) + L"%"};
        rows.push_back(r);
    };
    addRow(L"Wallpaper Engine", 5.0f, 120.0f, 3.0f);
    addRow(L"Widget Platform", 2.0f, 45.0f, 0.0f);
    addRow(L"Shell UI", 8.0f, 80.0f, 0.0f);
    addRow(L"Settings Framework", 0.1f, 5.0f, 0.0f);
    m_processTable.SetRows(rows);
}
