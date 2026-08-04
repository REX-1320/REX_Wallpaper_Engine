#pragma once
#include <d2d1_3.h>
#include <dwrite.h>
#include <memory>
#include <string>
#include <vector>
#include "GlowStyle.h"
#include "LiquidGlassTokens.hpp"
#include "Button.h"
#include "Card.h"
#include "Toggle.h"
#include "ProgressBar.h"
#include "SectionHeader.h"
#include "GlowTextInput.h"
#include "../modules/Sync/engine/SyncEngine.hpp"

namespace rex::shell {

/// CloudSyncPage renders the Settings > Cloud Sync UI workspace page (spec §9.1).
class CloudSyncPage {
public:
    CloudSyncPage(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, const GlowStyle& style,
                  std::shared_ptr<rex::sync::SyncEngine> syncEngine);
    ~CloudSyncPage() = default;

    void SetBounds(float x, float y, float w, float h);
    void HandleClick(float mx, float my);
    void Update(float dt);
    void Render(ID2D1RenderTarget* rt);

private:
    float m_x = 0, m_y = 0, m_width = 800, m_height = 600;
    GlowStyle m_style;
    LiquidGlassTokens m_glassTokens = LiquidGlassTokens::DefaultDark();
    std::shared_ptr<rex::sync::SyncEngine> m_syncEngine;

    ID2D1RenderTarget* m_rt = nullptr;
    IDWriteFactory* m_dwrite = nullptr;

    SectionHeader m_header;
    Card m_accountCard;
    Card m_storageCard;
    Card m_selectiveSyncCard;
    Card m_conflictCard;

    Button m_signInBtn;
    Button m_syncNowBtn;
    Button m_signOutBtn;

    Toggle m_profilesToggle;
    Toggle m_settingsToggle;
    Toggle m_wallpapersToggle;
    Toggle m_widgetsToggle;
    Toggle m_wifiToggle;

    ProgressBar m_storageProgress;

    GlowTextInput m_emailInput;
    GlowTextInput m_passwordInput;
};

} // namespace rex::shell
