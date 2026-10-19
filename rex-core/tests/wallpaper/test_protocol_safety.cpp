// ═══════════════════════════════════════════════════════════════════════════
// P2-17 regression tests: protocol hardening (R11 + R12)
// ═══════════════════════════════════════════════════════════════════════════
//
// R11: the throwing single-arg filesystem::exists() on hostile paths
// (over-long, embedded NUL, dead shares) threw filesystem_error →
// uncaught → terminate. SafeExists uses the error_code overload plus a
// length cap and never throws.
// R12: a zero-length length-prefix is a protocol violation, not EOF —
// it must answer `malformed frame` and continue, never tear the session
// down. An oversized prefix, however, has no trustworthy body boundary
// (audit 5.5 #3): it must classify as unrecoverable so the dispatcher
// tears the session down instead of reading payload as the next prefix.

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <string>

#include "wallpaper/runtime/FrameProtocol.hpp"
#include "wallpaper/runtime/SafePath.hpp"

using namespace rex::wallpaper;

TEST(FrameLengthTest, ZeroLengthIsEmptyButRecoverable) {
    // No body follows, so answering `malformed frame` keeps the stream
    // aligned (P2-17/R12).
    EXPECT_EQ(ClassifyFrameLength(0), FrameLengthVerdict::Empty);
}

TEST(FrameLengthTest, NormalLengthsAreOk) {
    EXPECT_EQ(ClassifyFrameLength(1), FrameLengthVerdict::Ok);
    EXPECT_EQ(ClassifyFrameLength(1024), FrameLengthVerdict::Ok);
    EXPECT_EQ(ClassifyFrameLength(kMaxFrameBytes), FrameLengthVerdict::Ok);
}

TEST(FrameLengthTest, OversizedLengthsAreUnrecoverable) {
    // Audit 5.5 #3: the body boundary cannot be trusted, so the dispatcher
    // must close the session instead of continuing into a desync.
    EXPECT_EQ(ClassifyFrameLength(kMaxFrameBytes + 1),
              FrameLengthVerdict::Oversized);
    EXPECT_EQ(ClassifyFrameLength(0xFFFFFFFFu),
              FrameLengthVerdict::Oversized);
}

TEST(SafeExistsTest, MissingFileIsFalse) {
    EXPECT_FALSE(
        SafeExists(std::filesystem::path("D:/definitely-not-here-xyz123.mp4")));
}

TEST(SafeExistsTest, EmptyPathIsFalse) {
    EXPECT_FALSE(SafeExists(std::filesystem::path()));
}

TEST(SafeExistsTest, OverLongPathIsFalseNotThrow) {
    // 40k chars: beyond NT limits. The throwing exists() overload dies
    // on these; SafeExists must answer false.
    const std::filesystem::path hostile(std::string(40000, 'a'));
    EXPECT_NO_THROW({
        EXPECT_FALSE(SafeExists(hostile));
    });
}

TEST(SafeExistsTest, EmbeddedNulIsFalseNotThrow) {
    const std::string raw("C:\\wallpaper\0quat", 18);
    EXPECT_NO_THROW({
        EXPECT_FALSE(SafeExists(std::filesystem::path(raw)));
    });
}

TEST(SafeExistsTest, RealFileIsTrue) {
    const auto tmp =
        std::filesystem::temp_directory_path() / "rex_safe_exists_probe.tmp";
    {
        FILE* f = nullptr;
        ASSERT_EQ(::fopen_s(&f, tmp.string().c_str(), "w"), 0);
        ASSERT_NE(f, nullptr);
        std::fputc('x', f);
        std::fclose(f);
    }
    EXPECT_TRUE(SafeExists(tmp));
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
}
