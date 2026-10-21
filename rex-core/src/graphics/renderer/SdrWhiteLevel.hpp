#pragma once

// ─── The OS's SDR white level ─────────────────────────────────────────────
//
// On an HDR display the OS knows how bright "SDR white" should be - it is
// the Windows "SDR content brightness" slider - and exposes it as a
// fixed-point multiplier of the 80-nit reference:
//
//     nits = (SDRWhiteLevel / 1000) * 80
//
// The wallpaper uses that value to scale SDR content on an HDR panel, so the
// picture follows the user's brightness choice instead of a hard-coded
// assumption. On an SDR display the OS has nothing to say (the value is
// meaningless there) and the caller keeps its fallback.
//
// Header-only on purpose: a new .cpp would need a CMakeLists edit, and the
// repo does not allow those.

#include <windows.h>
#include <wingdi.h>

#include <string>
#include <vector>

namespace rex::graphics {

/// The fixed-point conversion the OS documents for
/// DISPLAYCONFIG_SDR_WHITE_LEVEL: nits = (level / 1000) * 80, where 80 is
/// the SDR reference white. Pure, so the unit test can pin it.
inline float SdrWhiteLevelToNits(unsigned long level) {
    return (static_cast<float>(level) / 1000.0f) * 80.0f;
}

/// The SDR white level of the display showing `sourceDeviceName` (a DXGI
/// output name such as "\\.\DISPLAY1"), in nits.
///
/// Returns 0 when it cannot be read - an SDR-only display, an OS without the
/// API, or a name that matches no active path. The caller then keeps its
/// fallback (the engine uses 100 nits, the SDR diffuse white of ITU-R
/// BT.2408).
inline float QuerySdrWhiteLevelNits(const std::wstring& sourceDeviceName) {
    if (sourceDeviceName.empty()) {
        return 0.0f;
    }

    // QueryDisplayConfig takes six parameters (the topology id is the last
    // one); the mode-info array may be NULL when only the paths are wanted.
    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    DISPLAYCONFIG_TOPOLOGY_ID topologyId = {};
    if (QueryDisplayConfig(0, &pathCount, nullptr, &modeCount, nullptr,
                           &topologyId) != ERROR_SUCCESS ||
        pathCount == 0) {
        return 0.0f;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    modeCount = 0;
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(),
                           &modeCount, nullptr, &topologyId) != ERROR_SUCCESS) {
        return 0.0f;
    }

    for (const DISPLAYCONFIG_PATH_INFO& path : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (!DisplayConfigGetDeviceInfo(&source.header)) {
            continue;
        }
        if (_wcsicmp(source.viewGdiDeviceName, sourceDeviceName.c_str()) != 0) {
            continue;
        }

        DISPLAYCONFIG_SDR_WHITE_LEVEL level = {};
        level.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        level.header.size = sizeof(level);
        level.header.adapterId = path.sourceInfo.adapterId;
        level.header.id = path.sourceInfo.id;
        if (!DisplayConfigGetDeviceInfo(&level.header)) {
            return 0.0f;
        }
        if (level.SDRWhiteLevel == 0) {
            return 0.0f;   // the display does not report one
        }
        const float nits = SdrWhiteLevelToNits(level.SDRWhiteLevel);
        return (nits > 1.0f) ? nits : 0.0f;
    }
    return 0.0f;
}

}  // namespace rex::graphics

/// Is the display showing `sourceDeviceName` in an HDR mode?
///
/// The DXGI output's colour space does NOT answer this: in an HDR desktop
/// the DWM still presents SDR windows with a G22 colour space, so the
/// swapchain looks like SDR even though the display is in HDR. The OS's
/// advanced-colour state does answer it.
///
/// This matters because the DWM misreads a 10-bit UNORM swapchain in an HDR
/// desktop - the wallpaper came out green/magenta (the R and B channels
/// wrong) for every SDR file. The engine's HDR output path (PQ on an FP16
/// back buffer) is the one the compositor expects there.
inline bool QueryDisplayHdrEnabled(const std::wstring& sourceDeviceName) {
    if (sourceDeviceName.empty()) {
        return false;
    }
    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    DISPLAYCONFIG_TOPOLOGY_ID topologyId = {};
    if (QueryDisplayConfig(0, &pathCount, nullptr, &modeCount, nullptr,
                           &topologyId) != ERROR_SUCCESS ||
        pathCount == 0) {
        return false;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    modeCount = 0;
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(),
                           &modeCount, nullptr, &topologyId) != ERROR_SUCCESS) {
        return false;
    }
    for (const DISPLAYCONFIG_PATH_INFO& path : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (!DisplayConfigGetDeviceInfo(&source.header)) {
            continue;
        }
        if (_wcsicmp(source.viewGdiDeviceName, sourceDeviceName.c_str()) != 0) {
            continue;
        }
        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO state = {};
        state.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
        state.header.size = sizeof(state);
        state.header.adapterId = path.sourceInfo.adapterId;
        state.header.id = path.sourceInfo.id;
        if (!DisplayConfigGetDeviceInfo(&state.header)) {
            return false;
        }
        return state.advancedColorEnabled != 0;
    }
    return false;
}
