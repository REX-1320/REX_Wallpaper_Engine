#pragma once

// ─── Thread naming (diagnostic) ─────────────────────────────────────────
// Best-effort SetThreadDescription wrapper so pipeline threads show up with
// roles (REX-Decode / REX-Render) in ETW traces, debuggers and crash dumps
// instead of anonymous IDs. A failure is never an error: naming is purely
// observational and must not affect behavior.

#include <string>

#include <windows.h>
#include <processthreadsapi.h>

namespace rex::shared {

// Names the calling thread. Returns true when the OS accepted the name.
inline bool SetCurrentThreadName(const wchar_t* name) {
    if (!name || !*name) return false;
    return SUCCEEDED(SetThreadDescription(GetCurrentThread(), name));
}

// Reads back the calling thread's description (empty when unnamed or on
// error). Exists so tests can verify the round-trip.
inline std::wstring GetCurrentThreadName() {
    PWSTR raw = nullptr;
    if (FAILED(GetThreadDescription(GetCurrentThread(), &raw)) || !raw) {
        return L"";
    }
    std::wstring out(raw);
    LocalFree(raw);
    return out;
}

}  // namespace rex::shared
