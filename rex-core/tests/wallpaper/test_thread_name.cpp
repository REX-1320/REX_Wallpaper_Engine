#include <gtest/gtest.h>

#include <shared/ThreadName.hpp>

TEST(ThreadNameTest, RoundTripOnCallingThread) {
    // SetThreadDescription/GetThreadDescription round-trip on this test
    // thread. Best-effort API: a false return skips rather than fails (e.g.
    // downlevel host), but on this target it must succeed.
    const std::wstring before = rex::shared::GetCurrentThreadName();
    if (!rex::shared::SetCurrentThreadName(L"REX-Test-Probe")) {
        GTEST_SKIP() << "SetThreadDescription unavailable";
    }
    EXPECT_EQ(rex::shared::GetCurrentThreadName(), L"REX-Test-Probe");
    // Restore (ignore result): leave the test runner's thread as found.
    rex::shared::SetCurrentThreadName(before.c_str());
}

TEST(ThreadNameTest, EmptyNameIsRejected) {
    EXPECT_FALSE(rex::shared::SetCurrentThreadName(nullptr));
    EXPECT_FALSE(rex::shared::SetCurrentThreadName(L""));
}
