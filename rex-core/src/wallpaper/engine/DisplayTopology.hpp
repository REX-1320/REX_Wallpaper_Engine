#pragma once

// ─── Display-topology change detection (P0-2 / G3) ──────────────────────────
//
// CheckDisplayChanges used to compare only monitor count + primary W/H, so
// secondary-only moves, same-resolution primary swaps and DPI changes never
// reconciled (and a same-resolution OS HDR toggle never even reached the
// renderer). The predicate below compares everything rendering depends on.
//
// NOTE: a same-resolution OS HDR toggle changes no geometry, so it never
// flips this predicate — HDR is re-evaluated separately on every topology
// check via D3D12Renderer::RefreshHdrOutput (cheap: SwapChain bails out
// before any DXGI churn when nothing changed).

#include <vector>

#include <rex/wallpaper/IWallpaperEngine.hpp>

namespace rex::wallpaper {

/// True when the rendering-relevant topology changed: arrival/removal,
/// geometry move/resize, primary swap, or DPI change. Order-sensitive
/// (EnumDisplayMonitors order is deterministic per topology); a spurious
/// true only costs an idempotent refresh + an early-return Resize.
inline bool MonitorTopologyChanged(const std::vector<MonitorInfo>& before,
                                   const std::vector<MonitorInfo>& after) {
    if (before.size() != after.size()) return true;
    for (size_t i = 0; i < before.size(); ++i) {
        const MonitorInfo& a = before[i];
        const MonitorInfo& b = after[i];
        if (a.id != b.id || a.x != b.x || a.y != b.y || a.width != b.width ||
            a.height != b.height || a.isPrimary != b.isPrimary ||
            a.dpiScale != b.dpiScale) {
            return true;
        }
    }
    return false;
}

}  // namespace rex::wallpaper
