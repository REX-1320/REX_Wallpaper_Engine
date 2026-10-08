#pragma once

// ─── Single-runtime ownership (P3 / G5) ─────────────────────────────────────
//
// Two runtimes fight over the WorkerW layer (verified live: the second
// runtime's startup/shutdown silently kills the other). The first runtime
// owns a native named mutex; a second instance detects ERROR_ALREADY_EXISTS
// and refuses to start instead of fighting.

#include <windows.h>

namespace rex::wallpaper {

class RuntimeInstanceGuard {
  public:
    explicit RuntimeInstanceGuard(const wchar_t* name =
                                      L"Local\\REXWallpaperRuntime") {
        m_mutex = CreateMutexW(nullptr, TRUE, name);
        m_owned = (m_mutex != nullptr &&
                   GetLastError() != ERROR_ALREADY_EXISTS);
    }

    ~RuntimeInstanceGuard() {
        if (m_mutex) {
            if (m_owned) ReleaseMutex(m_mutex);
            CloseHandle(m_mutex);
        }
    }

    RuntimeInstanceGuard(const RuntimeInstanceGuard&) = delete;
    RuntimeInstanceGuard& operator=(const RuntimeInstanceGuard&) = delete;

    /// True when this instance owns the layer and may proceed.
    bool IsOwner() const { return m_owned; }

  private:
    HANDLE m_mutex = nullptr;
    bool m_owned = false;
};

}  // namespace rex::wallpaper
