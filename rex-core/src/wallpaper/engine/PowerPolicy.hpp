#pragma once

// ─── Battery-saver pause rule (P3 / G1) ─────────────────────────────────────
//
// Laptops must not decode/upload/present at full cadence on battery saver
// (or a critically low battery off AC). The engine polls
// GetSystemPowerStatus and pauses expensive work while this holds.
// Unknown states fail open (never pause on guesses).

namespace rex::wallpaper {

/// @param acOnline        wall power connected (ACLineStatus == 1; 255 unknown reads as online)
/// @param saverOn         OS battery saver active (SystemStatusFlag & 1)
/// @param batteryPercent  0..100, or -1 when unknown (255)
inline bool ShouldPauseForPower(bool acOnline, bool saverOn,
                                int batteryPercent) {
    if (saverOn) return true;
    if (!acOnline && batteryPercent >= 0 && batteryPercent <= 15) return true;
    return false;
}

}  // namespace rex::wallpaper
