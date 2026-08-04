#include "CloudSyncPage.hpp"
#include "graphics/effects/LiquidGlassBrush.hpp"

namespace rex::shell {

CloudSyncPage::CloudSyncPage(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, const GlowStyle& style,
                             std::shared_ptr<rex::sync::SyncEngine> syncEngine)
    : m_style(style), m_dwrite(dwrite), m_rt(rt), m_syncEngine(syncEngine),
      m_header(rt, dwrite, style),
      m_accountCard(rt, dwrite, style),
      m_storageCard(rt, dwrite, style),
      m_selectiveSyncCard(rt, dwrite, style),
      m_conflictCard(rt, dwrite, style),
      m_signInBtn(L"Sign In", rt, dwrite, style),
      m_syncNowBtn(L"Sync Now", rt, dwrite, style),
      m_signOutBtn(L"Sign Out", rt, dwrite, style),
      m_profilesToggle(rt, style),
      m_settingsToggle(rt, style),
      m_wallpapersToggle(rt, style),
      m_widgetsToggle(rt, style),
      m_wifiToggle(rt, style),
      m_storageProgress(rt, style),
      m_emailInput(L"user@example.com", rt, dwrite, style),
      m_passwordInput(L"••••••••", rt, dwrite, style)
{
    m_header.SetTitle(L"Cloud Sync");
    m_header.SetSubtitle(L"Encrypt and synchronize desktop environment across Windows devices");

    m_accountCard.SetTitle(L"Account Status");
    m_storageCard.SetTitle(L"Storage Usage (5 GB Quota)");
    m_selectiveSyncCard.SetTitle(L"Selective Sync Categories");
    m_conflictCard.SetTitle(L"Pending Conflicts");

    m_profilesToggle.SetChecked(true);
    m_settingsToggle.SetChecked(true);
    m_wallpapersToggle.SetChecked(true);
    m_widgetsToggle.SetChecked(true);
    m_wifiToggle.SetChecked(true);

    m_storageProgress.SetProgress(0.24f); // 1.2 GB of 5 GB used

    m_signInBtn.SetOnClick([this]() {
        if (m_syncEngine) {
            m_syncEngine->SignIn("user@example.com", "password123");
        }
    });

    m_syncNowBtn.SetOnClick([this]() {
        if (m_syncEngine) {
            m_syncEngine->SyncNow();
        }
    });

    m_signOutBtn.SetOnClick([this]() {
        if (m_syncEngine) {
            m_syncEngine->SignOut();
        }
    });
}

void CloudSyncPage::SetBounds(float x, float y, float w, float h) {
    m_x = x; m_y = y; m_width = w; m_height = h;

    m_header.SetPosition(x, y);

    float cardW = (w - 24.0f) / 2.0f;
    float cardH = 180.0f;

    m_accountCard.SetBounds(x, y + 50, cardW, cardH);
    m_storageCard.SetBounds(x + cardW + 16, y + 50, cardW, cardH);
    m_selectiveSyncCard.SetBounds(x, y + 246, cardW, 220);
    m_conflictCard.SetBounds(x + cardW + 16, y + 246, cardW, 220);

    m_signInBtn.SetPosition(x + 20, y + 160);
    m_signOutBtn.SetPosition(x + 130, y + 160);
    m_syncNowBtn.SetPosition(x + cardW + 36, y + 160);

    m_emailInput.SetPosition(x + 20, y + 90, cardW - 40);
    m_passwordInput.SetPosition(x + 20, y + 125, cardW - 40);

    m_storageProgress.SetBounds(x + cardW + 36, y + 110, cardW - 40, 12);

    m_profilesToggle.SetPosition(x + 20, y + 290);
    m_settingsToggle.SetPosition(x + 20, y + 325);
    m_wallpapersToggle.SetPosition(x + 20, y + 360);
    m_widgetsToggle.SetPosition(x + 20, y + 395);
    m_wifiToggle.SetPosition(x + cardW + 36, y + 290);
}

void CloudSyncPage::HandleClick(float mx, float my) {
    bool signedIn = m_syncEngine && m_syncEngine->GetState() != rex::sync::SyncState::SignedOut;

    if (!signedIn && m_signInBtn.HitTest(mx, my)) {
        if (auto cb = m_signInBtn.GetOnClick()) cb();
    } else if (signedIn) {
        if (m_signOutBtn.HitTest(mx, my)) {
            if (auto cb = m_signOutBtn.GetOnClick()) cb();
        }
        if (m_syncNowBtn.HitTest(mx, my)) {
            if (auto cb = m_syncNowBtn.GetOnClick()) cb();
        }
    }

    if (m_profilesToggle.HitTest(mx, my)) {
        m_profilesToggle.ToggleState();
        if (m_syncEngine) m_syncEngine->GetSettings().syncProfiles = m_profilesToggle.IsChecked();
    }
    if (m_settingsToggle.HitTest(mx, my)) {
        m_settingsToggle.ToggleState();
        if (m_syncEngine) m_syncEngine->GetSettings().syncSettings = m_settingsToggle.IsChecked();
    }
    if (m_wallpapersToggle.HitTest(mx, my)) {
        m_wallpapersToggle.ToggleState();
        if (m_syncEngine) m_syncEngine->GetSettings().syncWallpapers = m_wallpapersToggle.IsChecked();
    }
    if (m_widgetsToggle.HitTest(mx, my)) {
        m_widgetsToggle.ToggleState();
        if (m_syncEngine) m_syncEngine->GetSettings().syncWidgets = m_widgetsToggle.IsChecked();
    }
    if (m_wifiToggle.HitTest(mx, my)) {
        m_wifiToggle.ToggleState();
        if (m_syncEngine) m_syncEngine->GetSettings().wifiOnlyForFiles = m_wifiToggle.IsChecked();
    }
}

void CloudSyncPage::Update(float dt) {
    m_signInBtn.Update(dt);
    m_syncNowBtn.Update(dt);
    m_signOutBtn.Update(dt);
    m_accountCard.Update(dt);
    m_storageCard.Update(dt);
    m_selectiveSyncCard.Update(dt);
    m_conflictCard.Update(dt);
}

void CloudSyncPage::Render(ID2D1RenderTarget* rt) {
    if (!rt) return;

    m_header.Render(rt);

    m_accountCard.Render(rt);
    m_storageCard.Render(rt);
    m_selectiveSyncCard.Render(rt);
    m_conflictCard.Render(rt);

    bool signedIn = m_syncEngine && m_syncEngine->GetState() != rex::sync::SyncState::SignedOut;

    if (signedIn) {
        m_signOutBtn.Render(rt);
        m_syncNowBtn.Render(rt);
    } else {
        m_emailInput.Render(rt);
        m_passwordInput.Render(rt);
        m_signInBtn.Render(rt);
    }

    m_storageProgress.Render(rt);

    m_profilesToggle.Render(rt);
    m_settingsToggle.Render(rt);
    m_wallpapersToggle.Render(rt);
    m_widgetsToggle.Render(rt);
    m_wifiToggle.Render(rt);
}

} // namespace rex::shell
