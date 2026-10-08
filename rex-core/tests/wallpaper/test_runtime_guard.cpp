// ═══════════════════════════════════════════════════════════════════════════
// P3-G5 regression tests: single-runtime ownership + protocol version (G5)
// ═══════════════════════════════════════════════════════════════════════════
//
// Two rex-wallpaper-runtime.exe processes fight over the WorkerW layer
// (verified: a second runtime's startup/shutdown silently kills the other).
// A native named mutex refuses the second instance; an INIT protocolVersion
// handshake fails skew as a clear error instead of silent defaults.

#include <gtest/gtest.h>

#include <string>

#include "wallpaper/runtime/RuntimeGuard.hpp"
#include "wallpaper/runtime/FrameProtocol.hpp"

using namespace rex::wallpaper;

TEST(RuntimeGuardTest, FirstInstanceOwnsTheLayer) {
    RuntimeInstanceGuard guard(L"Local\\REXTestGuard1");
    EXPECT_TRUE(guard.IsOwner());
}

TEST(RuntimeGuardTest, SecondInstanceIsRefused) {
    RuntimeInstanceGuard first(L"Local\\REXTestGuard2");
    ASSERT_TRUE(first.IsOwner());
    RuntimeInstanceGuard second(L"Local\\REXTestGuard2");
    EXPECT_FALSE(second.IsOwner());
}

TEST(RuntimeGuardTest, DistinctNamesCoexist) {
    RuntimeInstanceGuard a(L"Local\\REXTestGuard3a");
    RuntimeInstanceGuard b(L"Local\\REXTestGuard3b");
    EXPECT_TRUE(a.IsOwner());
    EXPECT_TRUE(b.IsOwner());
}

TEST(RuntimeGuardTest, ReleaseAllowsReacquire) {
    {
        RuntimeInstanceGuard first(L"Local\\REXTestGuard4");
        ASSERT_TRUE(first.IsOwner());
    }
    RuntimeInstanceGuard second(L"Local\\REXTestGuard4");
    EXPECT_TRUE(second.IsOwner());
}

TEST(ProtocolVersionTest, CurrentVersionIsSupported) {
    EXPECT_TRUE(IsSupportedProtocolVersion(kProtocolVersion));
}

TEST(ProtocolVersionTest, OtherVersionsAreRejected) {
    EXPECT_FALSE(IsSupportedProtocolVersion(kProtocolVersion + 1));
    EXPECT_FALSE(IsSupportedProtocolVersion(0));
    EXPECT_FALSE(IsSupportedProtocolVersion(-1));
}
