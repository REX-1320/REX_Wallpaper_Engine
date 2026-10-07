#pragma once

// ─── Client frame-surface lifecycle (R13) ────────────────────────────────────
//
// The host process sends its PID via SET_CLIENT_PID; the runtime duplicates
// the shared fence + pool-slot NT handles into the host so it can call
// ID3D12Device::OpenSharedHandle on its own device. Handles are
// re-duplicated whenever the video surface generation changes.
//
// Two bugs lived here:
//   1. DuplicateHandle BOOL returns were discarded and `duplicated` was set
//      unconditionally — a dead client yielded NULL/stale handles that were
//      then serialized as valid.
//   2. The client process was never revalidated and CloseClientSurface never
//      touched pid/process, so SHUTDOWN/EOF leaked the process handle and a
//      dead client stayed bound forever.

#include <windows.h>

#include <cstdint>

namespace rex::wallpaper {

inline constexpr UINT kMaxClientPreviewSlots = 8;

struct ClientSurfaceState {
    DWORD pid = 0;
    HANDLE process = nullptr;  // PROCESS_DUP_HANDLE handle to the host
    uint64_t generation = 0;
    UINT slotCount = 0;
    HANDLE fence = nullptr;
    HANDLE slots[kMaxClientPreviewSlots] = {};
    bool duplicated = false;  // true when handles were DuplicateHandle'd
                              // into this process (we own them)
};

/// Liveness gate: an exited client leaves a signaled process handle.
/// Duplicating into it yields stale handles, so the caller must detach
/// instead of serializing them. Uses GetExitCodeProcess (not a wait) so it
/// works with the PROCESS_QUERY_LIMITED_INFORMATION rights the client
/// binding is opened with — a wait needs SYNCHRONIZE and would fail with
/// WAIT_FAILED, wrongly reporting every client as dead.
inline bool IsProcessAlive(HANDLE process) {
    if (!process) return false;
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(process, &exitCode)) return false;
    return exitCode == STILL_ACTIVE;
}

/// Release the duplicated (or forget the process-local) surface handles.
/// Never touches pid/process: regeneration re-duplicates into the same
/// client, so the binding must survive. Use DetachClient to drop the
/// client entirely.
inline void CloseSurfaceHandles(ClientSurfaceState& s) {
    if (s.duplicated) {
        if (s.fence) {
            CloseHandle(s.fence);
            s.fence = nullptr;
        }
        for (UINT i = 0; i < kMaxClientPreviewSlots; ++i) {
            if (s.slots[i]) {
                CloseHandle(s.slots[i]);
                s.slots[i] = nullptr;
            }
        }
    } else {
        s.fence = nullptr;
        for (UINT i = 0; i < kMaxClientPreviewSlots; ++i) {
            s.slots[i] = nullptr;
        }
    }
    s.slotCount = 0;
    s.generation = 0;
    s.duplicated = false;
}

/// Drop the client entirely: surface handles plus the process binding.
inline void DetachClient(ClientSurfaceState& s) {
    CloseSurfaceHandles(s);
    if (s.process) {
        CloseHandle(s.process);
        s.process = nullptr;
    }
    s.pid = 0;
}

/// Every duplicated handle must be real before it is serialized to the host.
/// A NULL fence or slot means a duplication failed — the caller must drop
/// the surface and answer ok:false instead of sending a valid-looking 0x0.
inline bool ValidateDuplicatedSurface(HANDLE fence, HANDLE const* slots,
                                      UINT slotCount) {
    if (!fence) return false;
    for (UINT i = 0; i < slotCount; ++i) {
        if (!slots[i]) return false;
    }
    return true;
}

}  // namespace rex::wallpaper
