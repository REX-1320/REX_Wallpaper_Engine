#pragma once

#include <d3d12.h>

namespace rex::graphics {

/// Output dither: the amplitude rule, in one place.
///
/// The SDR back buffer is a UNORM target, so the final write quantises the
/// encoded signal into whole codes - 1024 levels on the 10-bit swapchain
/// (R10G10B10A2, mpv's window format), 256 on the 8-bit fallback. Truncating
/// a smooth gradient into discrete steps is the banding the colour TODO
/// filed; mpv dithers by default (--dither=fruit, a blue-noise template).
///
/// The dither is ONE CODE of the output depth, so the rule is per target
/// format and lives here: the pixel shader is compiled per pipeline state
/// with the depth as a compile-time constant (TARGET_BITS), and the shader
/// divides by (2^TARGET_BITS - 1). A float target (the FP16 HDR output) is
/// not quantised and must not dither.
///
/// Pinned by tests/wallpaper/test_color_pipeline.cpp.
constexpr int OutputDitherBits(DXGI_FORMAT rtvFormat) {
    switch (rtvFormat) {
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            return 10;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
            return 8;
        default:
            return 0;   // FP16 / other float targets: not quantised
    }
}

}  // namespace rex::graphics
