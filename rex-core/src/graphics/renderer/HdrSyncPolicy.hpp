#pragma once

// ─── HDR output sync plan (P0-2 / R16) ──────────────────────────────────────
//
// The renderer cached m_isHdrOutput once at Initialize and created the HDR
// PSO only when starting HDR: a later switch left the flag stale with
// nothing to bind (SDR PSO on an R16F/PQ swapchain). RefreshHdrOutput
// re-reads the live swapchain state and applies this plan.

namespace rex::graphics {

/// What a refresh must do after re-reading the live swapchain HDR state.
struct HdrSyncAction {
    /// Cached flag != live state: update m_isHdrOutput (drives PSO choice
    /// and color constants).
    bool updateFlag = false;
    /// HDR output and no HDR PSO yet (started SDR, or PSO was dropped):
    /// create the R16F pipeline state before anything binds it.
    bool createHdrPso = false;
};

/// Decide the refresh actions.
///   @param cachedHdr   m_isHdrOutput before the refresh
///   @param liveHdr    m_swapChain.IsHDR() after re-evaluation
///   @param hdrPsoReady m_pipelineStateHdr is non-null
inline HdrSyncAction PlanHdrSync(bool cachedHdr, bool liveHdr,
                                 bool hdrPsoReady) {
    HdrSyncAction action;
    action.updateFlag = (cachedHdr != liveHdr);
    action.createHdrPso = liveHdr && !hdrPsoReady;
    return action;
}

/// Decide the cached HDR flag after a FAILED HDR-PSO creation.
/// The refresh first tries to bring flag and buffers back together by
/// reverting the swapchain to SDR (forced-SDR refresh); the flag may only
/// claim SDR once the swapchain actually is SDR. Clearing the flag over
/// an R16F/PQ back buffer would bind the SDR pipeline with sRGB math onto
/// a PQ surface (format mismatch + wrong transform) — audit 2026-10-17,
/// #4. While the swapchain is still HDR the flag stays true so the PSO
/// choice keeps matching the real buffers and the next refresh retries
/// the missing PSO.
inline bool HdrFlagAfterPsoFailure(bool swapchainHdrAfterRevert) {
    return swapchainHdrAfterRevert;
}

}  // namespace rex::graphics
