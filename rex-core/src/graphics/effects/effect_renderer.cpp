#include "graphics/effects/effect_renderer.h"
#include <cstdio>

bool Dwm22H2EffectRenderer::ApplyBackdrop(HWND target, BackdropType type) {
    DWORD backdropType = DWMWA_SYSTEMBACKDROP_TYPE;
    int value = 0;

    switch (type) {
    case BackdropType::Mica:    value = 2; break;
    case BackdropType::Acrylic: value = 3; break;
    case BackdropType::Tabbed:  value = 4; break;
    default: return false;
    }

    HRESULT hr = DwmSetWindowAttribute(target, backdropType, &value, sizeof(value));
    if (SUCCEEDED(hr)) {
        printf("[REX] DWM backdrop applied (type: %d)\n", static_cast<int>(type));
        return true;
    }

    printf("[REX] DWM backdrop failed (error: 0x%08lx)\n", hr);
    return false;
}

bool Dwm22H2EffectRenderer::SupportsBackdropType(BackdropType type) const {
    return type != BackdropType::None;
}

bool LegacyAccentEffectRenderer::ApplyBackdrop(HWND target, BackdropType type) {
    struct ACCENTPOLICY {
        DWORD accentState;
        DWORD accentFlags;
        DWORD gradientColor;
        DWORD animationId;
    };

    struct WINCOMPATTRDATA {
        DWORD attribute;
        ACCENTPOLICY* pData;
        SIZE_T dataSize;
    };

    enum ACCENT_STATE {
        ACCENT_DISABLED = 0,
        ACCENT_ENABLE_GRADIENT = 1,
        ACCENT_ENABLE_TRANSPARENTGRADIENT = 2,
        ACCENT_ENABLE_BLURBEHIND = 3,
        ACCENT_ENABLE_ACRYLICBLURBEHIND = 4,
        ACCENT_ENABLE_HOSTBACKDROP = 5
    };

    ACCENTPOLICY policy = {};
    policy.accentState = (type == BackdropType::Acrylic)
        ? ACCENT_ENABLE_ACRYLICBLURBEHIND
        : ACCENT_ENABLE_BLURBEHIND;
    policy.gradientColor = 0xCC000000;

    WINCOMPATTRDATA data = {};
    data.attribute = 19;
    data.pData = &policy;
    data.dataSize = sizeof(policy);

    using SetWindowCompositionAttribute_t = BOOL(WINAPI*)(HWND, WINCOMPATTRDATA*);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return false;

    auto func = reinterpret_cast<SetWindowCompositionAttribute_t>(
        GetProcAddress(user32, "SetWindowCompositionAttribute"));
    if (!func) return false;

    return func(target, &data) != FALSE;
}

bool LegacyAccentEffectRenderer::SupportsBackdropType(BackdropType type) const {
    return type == BackdropType::Acrylic;
}
