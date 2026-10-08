#pragma once

// ─── Throw-free existence check (P2-17 / R11) ───────────────────────────────
//
// The single-arg filesystem::exists() throws filesystem_error on hostile
// inputs (over-long paths, embedded NULs, dead shares) — uncaught in the
// runtime that meant terminate/abort instead of `ok:false file not found`.
// SafeExists uses the error_code overload plus an NT length cap and never
// throws.

#include <filesystem>
#include <system_error>

namespace rex::wallpaper {

/// Longest native path Windows can address (MAX_PATH extended form).
inline constexpr size_t kMaxNativePathChars = 32767;

/// Existence check that cannot throw. Over-long paths cannot exist.
inline bool SafeExists(const std::filesystem::path& p) {
    if (p.native().size() > kMaxNativePathChars) return false;
    std::error_code ec;
    return std::filesystem::exists(p, ec);
}

}  // namespace rex::wallpaper
