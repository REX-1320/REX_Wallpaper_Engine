#pragma once
#include <windows.h>

namespace rex::shell {

enum class GlassPerformanceMode {
    Full,       ///< Full liquid glass effects (blur, noise, dynamic tint, spring motion).
    Reduced,    ///< Reduced blur radius, static blur (no blur animations), simple shadows.
    Minimal     ///< Solid surface colors, high-contrast, no blur.
};

/// AccessibilityDetector checks system settings for High Contrast, Reduced Motion,
/// and Battery Saver mode to adjust glass rendering quality and performance (spec §4.4).
class AccessibilityDetector {
public:
    AccessibilityDetector() = default;

    static bool IsHighContrastActive() {
        HIGHCONTRASTW hc = { sizeof(HIGHCONTRASTW) };
        if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0)) {
            return (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
        }
        return false;
    }

    static bool IsReducedMotionActive() {
        BOOL clientAnimation = TRUE;
        if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &clientAnimation, 0)) {
            return !clientAnimation;
        }
        return false;
    }

    static bool IsBatterySaverActive() {
        SYSTEM_POWER_STATUS status = {};
        if (GetSystemPowerStatus(&status)) {
            return status.SystemStatusFlag == 1 || status.ACLineStatus == 0;
        }
        return false;
    }

    static GlassPerformanceMode GetPerformanceMode() {
        if (IsHighContrastActive()) {
            return GlassPerformanceMode::Minimal;
        }
        if (IsBatterySaverActive() || IsReducedMotionActive()) {
            return GlassPerformanceMode::Reduced;
        }
        return GlassPerformanceMode::Full;
    }
};

} // namespace rex::shell
