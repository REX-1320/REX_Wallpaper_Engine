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
#include <limits>
#include <string>

#include "wallpaper/runtime/FrameProtocol.hpp"
#include "wallpaper/runtime/Json.hpp"
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

// ─── Audit 5.5 #5: numeric coercion safety ──────────────────────────────────
// The JSON parser accepts 1e999 (finite grammar, inf value). The old
// `static_cast<int>(AsNumber(...))` was UB on that input; AsInt/AsFloat must
// reject non-finite and clamp finite out-of-range values instead.

TEST(JsonNumericTest, NonFiniteRejectedWithDefault) {
    auto j = rex::runtime::Json::Parse("1e999");
    ASSERT_TRUE(j.has_value());
    EXPECT_EQ(j->AsInt(7), 7);
    EXPECT_FLOAT_EQ(j->AsFloat(2.5f), 2.5f);
}

TEST(JsonNumericTest, OutOfRangeIntClamps) {
    auto big = rex::runtime::Json::Parse("9999999999");
    ASSERT_TRUE(big.has_value());
    EXPECT_EQ(big->AsInt(0), (std::numeric_limits<int>::max)());
    auto small = rex::runtime::Json::Parse("-9999999999");
    ASSERT_TRUE(small.has_value());
    EXPECT_EQ(small->AsInt(0), (std::numeric_limits<int>::min)());
}

TEST(JsonNumericTest, OutOfRangeFloatClamps) {
    auto j = rex::runtime::Json::Parse("1e39");
    ASSERT_TRUE(j.has_value());
    EXPECT_EQ(j->AsFloat(0.0f), (std::numeric_limits<float>::max)());
}

TEST(JsonNumericTest, NormalValuesPassThrough) {
    auto i = rex::runtime::Json::Parse("42");
    ASSERT_TRUE(i.has_value());
    EXPECT_EQ(i->AsInt(-1), 42);
    auto f = rex::runtime::Json::Parse("3.5");
    ASSERT_TRUE(f.has_value());
    EXPECT_FLOAT_EQ(f->AsFloat(), 3.5f);
}

TEST(JsonNumericTest, NonNumericKindsFallBackToDefault) {
    // An object (or null) is not coercible: the caller's default is returned.
    auto o = rex::runtime::Json::Parse("{\"a\":1}");
    ASSERT_TRUE(o.has_value());
    EXPECT_EQ(o->AsInt(9), 9);
    EXPECT_FLOAT_EQ(o->AsFloat(1.5f), 1.5f);
}
