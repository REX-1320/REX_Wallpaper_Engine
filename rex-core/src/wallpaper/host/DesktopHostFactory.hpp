#pragma once

#include "IDesktopHost.hpp"
#include <memory>
#include <string>

namespace rex::wallpaper::host {

// ─── DesktopHostFactory ─────────────────────────────────────────────────────
//
// Creates IDesktopHost instances using a strategy chain.
//
// ## Strategy chain (CreateBestAvailable)
//
//   ┌─────────────┐
//   │  Try WorkerW │ ──→ succeeds ──→ return WorkerWHost
//   └─────────────┘
//          │ fails (validation error)
//          ▼
//   ┌─────────────┐
//   │ Try Progman  │ ──→ succeeds ──→ return ProgmanHost
//   └─────────────┘
//          │ fails
//          ▼
//   ┌──────────────────────────────┐
//   │ Return nullptr (fatal error) │
//   └──────────────────────────────┘
//
// ## Extensibility
//
// Adding a new strategy (e.g., DirectCompositionHost):
//
//   1. Implement IDesktopHost.
//   2. Add the try block in CreateBestAvailable().
//   3. Done. No changes to Engine or any consumer.
//
// ## Logging
//
// Every attempt and its reason for failure are logged, so the
// application's log contains a complete audit trail:
//
//   [DesktopHost] Trying WorkerW...
//   [WorkerWHost] WorkerW too small: 132×37 (desktop 2560×1440)
//   [DesktopHost] ✗ WorkerW failed: WorkerW too small...
//   [DesktopHost] Trying Progman...
//   [ProgmanHost] Progman validated: 0x1234 (2560×1440)
//   [DesktopHost] ✓ Progman acquired (2560×1440)

class DesktopHostFactory {
public:
    /// Create the best available desktop host.
    ///
    /// Tries each strategy in order (WorkerW → Progman → ...). Returns
    /// the first strategy whose Acquire() succeeds, or nullptr if all
    /// strategies fail. Each failure is logged with the specific reason.
    static std::unique_ptr<IDesktopHost> CreateBestAvailable();

    /// Create a specific host by type name.
    ///
    /// Supported types:
    ///   "WorkerW"  — WorkerWHost (classic WorkerW technique)
    ///   "Progman"  — ProgmanHost (Progman child-window technique)
    ///
    /// Returns nullptr for unknown types. The caller is responsible
    /// for calling Acquire() on the returned object.
    static std::unique_ptr<IDesktopHost> Create(const std::string& type);

private:
    DesktopHostFactory() = delete; // static utility class only
};

} // namespace rex::wallpaper::host
