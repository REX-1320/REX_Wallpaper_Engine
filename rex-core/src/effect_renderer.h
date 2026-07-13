#pragma once
#include <windows.h>
#include <dwmapi.h>
#include <string>

enum class BackdropType { None, Mica, Acrylic, Tabbed };

class IEffectRenderer {
public:
    virtual ~IEffectRenderer() = default;
    virtual bool ApplyBackdrop(HWND target, BackdropType type) = 0;
    virtual bool SupportsBackdropType(BackdropType type) const = 0;
};

class Dwm22H2EffectRenderer : public IEffectRenderer {
public:
    bool ApplyBackdrop(HWND target, BackdropType type) override;
    bool SupportsBackdropType(BackdropType type) const override;
};

class LegacyAccentEffectRenderer : public IEffectRenderer {
public:
    bool ApplyBackdrop(HWND target, BackdropType type) override;
    bool SupportsBackdropType(BackdropType type) const override;
};
