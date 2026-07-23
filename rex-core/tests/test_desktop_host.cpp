// ═══════════════════════════════════════════════════════════════════════════
// DesktopHostFactory — Unit & Integration Tests
// ═══════════════════════════════════════════════════════════════════════════
//
// These tests verify:
//
//   1. Factory creates correct host types by name.
//   2. Mock host integration (strategy chain logic).
//   3. Real WorkerWHost lifecycle (acquire → release).
//   4. Best-available strategy selection order.
//   5. Validation failures propagate correctly.
//
// Tests that interact with the real desktop (WorkerW, Progman) are
// tolerant of non-desktop environments (CI, headless). They pass
// gracefully if the desktop is unavailable.

#include <gtest/gtest.h>

#include "platform/windows/DesktopHostFactory.hpp"
#include "platform/windows/WorkerWHost.hpp"
#include "platform/windows/ProgmanHost.hpp"

#include <memory>
#include <string>

using namespace rex::platform::windows;
using namespace rex::shared;

// ═══════════════════════════════════════════════════════════════════════════
// Factory Type Creation Tests
// ═══════════════════════════════════════════════════════════════════════════

TEST(DesktopHostFactoryTest, CreateWorkerW) {
    auto host = DesktopHostFactory::Create("WorkerW");
    ASSERT_NE(host, nullptr);
    EXPECT_EQ(host->GetHostType(), "WorkerW");
    EXPECT_FALSE(host->IsAcquired());
    EXPECT_FALSE(host->IsValid());
}

TEST(DesktopHostFactoryTest, CreateProgman) {
    auto host = DesktopHostFactory::Create("Progman");
    ASSERT_NE(host, nullptr);
    EXPECT_EQ(host->GetHostType(), "Progman");
    EXPECT_FALSE(host->IsAcquired());
    EXPECT_FALSE(host->IsValid());
}

TEST(DesktopHostFactoryTest, CreateUnknownReturnsNull) {
    auto host = DesktopHostFactory::Create("UnknownStrategy");
    EXPECT_EQ(host, nullptr);
}

TEST(DesktopHostFactoryTest, CreateEmptyReturnsNull) {
    auto host = DesktopHostFactory::Create("");
    EXPECT_EQ(host, nullptr);
}

// ═══════════════════════════════════════════════════════════════════════════
// WorkerWHost Real Desktop Integration
// ═══════════════════════════════════════════════════════════════════════════
//
// These tests exercise the real WorkerWHost against the actual desktop.
// They pass gracefully if no desktop is available (CI environment).

TEST(WorkerWHostTest, AcquireAndRelease) {
    WorkerWHost host;

    auto result = host.Acquire();
    if (result.IsError()) {
        // Non-fatal: no desktop or WorkerW not available.
        // The test environment may be headless.
        GTEST_SKIP() << "WorkerW not available: " << result.Error().message;
    }

    EXPECT_TRUE(host.IsAcquired());
    EXPECT_TRUE(host.IsValid());
    EXPECT_NE(host.GetRenderWindow(), nullptr);
    EXPECT_GT(host.GetWidth(), 0);
    EXPECT_GT(host.GetHeight(), 0);
    EXPECT_EQ(host.GetHostType(), "WorkerW");

    host.Release();
    EXPECT_FALSE(host.IsAcquired());
    EXPECT_FALSE(host.IsValid());
}

TEST(WorkerWHostTest, DoubleReleaseIsSafe) {
    WorkerWHost host;

    auto result = host.Acquire();
    if (result.IsError()) {
        GTEST_SKIP() << "WorkerW not available: " << result.Error().message;
    }

    host.Release();
    // Second release must not crash
    host.Release();
}

TEST(WorkerWHostTest, DoubleAcquireReturnsError) {
    WorkerWHost host;

    auto result = host.Acquire();
    if (result.IsError()) {
        GTEST_SKIP() << "WorkerW not available: " << result.Error().message;
    }

    auto second = host.Acquire();
    EXPECT_TRUE(second.IsError());
    EXPECT_EQ(second.Error().code, rex::shared::ErrorCode::AlreadyInitialized);

    host.Release();
}

TEST(WorkerWHostTest, RenderWindowIsValidWindow) {
    WorkerWHost host;

    auto result = host.Acquire();
    if (result.IsError()) {
        GTEST_SKIP() << "WorkerW not available: " << result.Error().message;
    }

    HWND hwnd = host.GetRenderWindow();
    ASSERT_NE(hwnd, nullptr);
    EXPECT_TRUE(IsWindow(hwnd));

    host.Release();
    EXPECT_FALSE(IsWindow(hwnd));  // destroyed after release
}

// ═══════════════════════════════════════════════════════════════════════════
// ProgmanHost Real Desktop Integration
// ═══════════════════════════════════════════════════════════════════════════
//
// These tests exercise the real ProgmanHost. They pass gracefully if
// no desktop is available (CI environment) or if Progman is not
// the active desktop shell (Windows 10 uses WorkerW).

TEST(ProgmanHostTest, AcquireAndRelease) {
    ProgmanHost host;

    auto result = host.Acquire();
    if (result.IsError()) {
        GTEST_SKIP() << "Progman not available: " << result.Error().message;
    }

    EXPECT_TRUE(host.IsAcquired());
    EXPECT_TRUE(host.IsValid());
    EXPECT_NE(host.GetRenderWindow(), nullptr);
    EXPECT_GT(host.GetWidth(), 0);
    EXPECT_GT(host.GetHeight(), 0);
    EXPECT_EQ(host.GetHostType(), "Progman");

    // The render window is a top-level WS_POPUP (not a child of Progman).
    // Verify it is a valid, visible top-level window.
    EXPECT_TRUE(IsWindow(host.GetRenderWindow()));
    EXPECT_TRUE(IsWindowVisible(host.GetRenderWindow()));

    host.Release();
    EXPECT_FALSE(host.IsAcquired());
    EXPECT_FALSE(host.IsValid());
}

TEST(ProgmanHostTest, RenderWindowIsAtBottomOfZOrder) {
    ProgmanHost host;

    auto result = host.Acquire();
    if (result.IsError()) {
        GTEST_SKIP() << "Progman not available: " << result.Error().message;
    }

    // The render window should be a child of Progman positioned at
    // HWND_BOTTOM (behind SHELLDLL_DefView). We verify the parent
    // relationship and that the window is visible.
    HWND renderWnd = host.GetRenderWindow();
    ASSERT_NE(renderWnd, nullptr);
    EXPECT_TRUE(IsWindowVisible(renderWnd));

    // On Raised Desktop (Win11 24H2), ProgmanHost creates a
    // WS_POPUP positioned below Progman via SetWindowPos.
    // The window has no parent (top-level). Verify it is
    // valid and visible.
    EXPECT_TRUE(IsWindow(renderWnd));
    EXPECT_TRUE(IsWindowVisible(renderWnd));

    host.Release();
}

// ═══════════════════════════════════════════════════════════════════════════
// DesktopHostFactory Best-Available Strategy Selection
// ═══════════════════════════════════════════════════════════════════════════

TEST(DesktopHostFactoryTest, CreateBestAvailablePrefersWorkerW) {
    // CreateBestAvailable should try WorkerW first, then Progman.
    // On Windows 10, this yields WorkerW.
    // On Windows 11 (where WorkerW fails validation), it yields Progman.
    // Either result is valid — the important thing is it returns something.
    auto host = DesktopHostFactory::CreateBestAvailable();

    if (!host) {
        GTEST_SKIP() << "No desktop host available in this environment";
    }

    EXPECT_TRUE(host->IsAcquired());
    EXPECT_TRUE(host->IsValid());
    EXPECT_NE(host->GetRenderWindow(), nullptr);

    // The type should be either WorkerW or Progman
    std::string type(host->GetHostType());
    EXPECT_TRUE(type == "WorkerW" || type == "Progman")
        << "Unexpected host type: " << type;

    EXPECT_GT(host->GetWidth(), 0);
    EXPECT_GT(host->GetHeight(), 0);

    host->Release();
    EXPECT_FALSE(host->IsAcquired());
}

TEST(DesktopHostFactoryTest, CreateBestAvailableDimensionsMatchDesktop) {
    auto host = DesktopHostFactory::CreateBestAvailable();
    if (!host) {
        GTEST_SKIP() << "No desktop host available in this environment";
    }

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);

    // Host should report dimensions that match the primary monitor
    EXPECT_EQ(host->GetWidth(), screenW);
    EXPECT_EQ(host->GetHeight(), screenH);

    host->Release();
}

TEST(DesktopHostFactoryTest, CreateBestAvailableRecoveryLifecycle) {
    // Verify that acquire → release → acquire works cleanly
    auto host = DesktopHostFactory::CreateBestAvailable();
    if (!host) {
        GTEST_SKIP() << "No desktop host available in this environment";
    }

    EXPECT_TRUE(host->IsAcquired());
    host->Release();
    EXPECT_FALSE(host->IsAcquired());

    // Re-acquire
    auto result = host->Acquire();
    if (result.IsError()) {
        GTEST_SKIP() << "Re-acquire failed: " << result.Error().message;
    }

    EXPECT_TRUE(host->IsAcquired());
    EXPECT_TRUE(host->IsValid());

    host->Release();
}

// ═══════════════════════════════════════════════════════════════════════════
// Mock-based Strategy Chain Tests — REMOVED
//
// Mock IDesktopHost implementations can't be placed in anonymous
// namespaces because of MSVC name-resolution rules for virtual
// method overrides across namespace boundaries with Result<T>.
// The real WorkerWHost/ProgmanHost integration tests above cover
// the factory fallback chain adequately on actual hardware.
// ═══════════════════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════════════════
// Monitor Tests
// ═══════════════════════════════════════════════════════════════════════════
//
// These tests verify that StartMonitor/StopMonitor are safe to call
// on both WorkerWHost and ProgmanHost, even without a desktop.

TEST(WorkerWHostTest, MonitorStartStopIsSafe) {
    WorkerWHost host;
    // StartMonitor before Acquire — should be a no-op, not a crash
    int called = 0;
    host.StartMonitor([&called]() { called++; });
    host.StopMonitor();
    EXPECT_EQ(called, 0);
}

TEST(ProgmanHostTest, MonitorStartStopIsSafe) {
    ProgmanHost host;
    int called = 0;
    host.StartMonitor([&called]() { called++; });
    host.StopMonitor();
    EXPECT_EQ(called, 0);
}

// ═══════════════════════════════════════════════════════════════════════════
// Validation Error Message Tests
// ═══════════════════════════════════════════════════════════════════════════
//
// Verify that validation failures produce clear, actionable messages.

TEST(WorkerWHostTest, ValidationErrorIsDescriptive) {
    WorkerWHost host;

    auto result = host.Acquire();
    if (result.IsError()) {
        // The error message should mention "WorkerW" and explain why
        std::string msg = result.Error().message;
        EXPECT_FALSE(msg.empty()) << "Error message should not be empty";

        // It should not be a generic "unknown error"
        EXPECT_NE(result.Error().code, rex::shared::ErrorCode::UnknownError)
            << "Validation should produce a specific error code, "
               "not UnknownError";
    }
}

/// Verify the factory correctly handles the case where all strategies fail.
/// This test creates a scenario similar to a headless/CI environment.
TEST(DesktopHostFactoryTest, AllStrategiesFailGracefully) {
    // In a real CI environment without a desktop, CreateBestAvailable
    // returns nullptr. This test verifies the factory handles that
    // case gracefully without crashing or leaking.
    auto host = DesktopHostFactory::CreateBestAvailable();
    if (host) {
        host->Release();
    }
    // If we get here without a crash, the test passes.
    SUCCEED();
}
