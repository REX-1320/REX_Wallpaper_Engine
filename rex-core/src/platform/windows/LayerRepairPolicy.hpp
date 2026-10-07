#pragma once

namespace rex::platform::windows {

// ─── Layer repair policy (BUG-001) ──────────────────────────────────────────
//
// The wallpaper layer invariant check runs every few seconds and used to treat
// every failure the same way: destroy the render window, destroy the synthetic
// WorkerW, ask Explorer to spawn WorkerWs (4 sends, 1 s timeout each), rebuild,
// then fire the recovery callback so the engine reloads the whole renderer.
// A layer that was merely MISPOSITIONED — pushed above the desktop icons by an
// Explorer z-order shuffle — therefore cost a visible jump plus about a second
// of frozen desktop input, exactly the reported symptom.
//
// The cheap repair is to move the existing, still-alive layer back below the
// icons. Only a dead HWND (or a re-anchor that does not verify) earns the
// destructive rebuild.

enum class LayerRepair {
    None,      ///< layer verified — nothing to do
    Reanchor,  ///< windows alive, z-order wrong — reposition only, no rebuild
    Rebuild,   ///< a window is gone (or re-anchor failed) — full reacquire
};

/// Decide how to repair the wallpaper layer.
///   @param renderWindowAlive  the render HWND still exists
///   @param workerWAlive       the wallpaper layer HWND still exists
///   @param layerValid         VerifyWallpaperLayer() passed
///   @param reanchorFailed     a re-anchor was attempted and did not verify
inline LayerRepair ChooseLayerRepair(bool renderWindowAlive, bool workerWAlive,
                                     bool layerValid, bool reanchorFailed) {
    // A dead HWND always wins: never trust a stale "valid" answer over it.
    if (!renderWindowAlive || !workerWAlive) return LayerRepair::Rebuild;
    if (layerValid) return LayerRepair::None;
    return reanchorFailed ? LayerRepair::Rebuild : LayerRepair::Reanchor;
}

/// WM_SPAWNWORKERW (0x052C) makes Explorer's UI thread rebuild desktop layers
/// and costs up to ~4 s of shell time. Only ask for it when the shell may not
/// have a usable WorkerW yet; a live shell that merely reordered its layers
/// must never trigger it.
inline bool NeedsWorkerWSpawn(bool hostAlive) {
    return !hostAlive;
}

/// Outcome of a synthetic-layer transition across ReacquireWorkerW.
struct SyntheticTransition {
    /// The previous layer was synthetic, i.e. our window: destroy it before
    /// discovery. A real Explorer WorkerW is never ours — leave it alone.
    bool destroyOld = false;
    /// What the flag must become. It follows the NEW discovery, never the
    /// history: a stale `true` makes VerifyWallpaperLayer demand
    /// WS_EX_TRANSPARENT | WS_EX_NOACTIVATE on Explorer's own WorkerW, so a
    /// synthetic -> real transition would fail verification on every repair
    /// forever and the wallpaper would never come back.
    bool synthetic = false;
};

/// Plan the synthetic-layer handoff for a rebuild.
///   @param wasSynthetic  m_syntheticWorkerW before the rebuild
///   @param discoveryCreatedSynthetic  the new discovery created a synthetic
///          layer (FindBlankWorkerW sets the flag itself when it does)
inline SyntheticTransition PlanSyntheticTransition(
    bool wasSynthetic, bool discoveryCreatedSynthetic) {
    return SyntheticTransition{ wasSynthetic, discoveryCreatedSynthetic };
}

} // namespace rex::platform::windows
