// Decoder inventory for the SHIPPED avcodec binary.
//
// The roadmap requires: "FFmpeg's enabled decoders depend on how that build was
// configured, so REX must test the actual shipped binary rather than assume
// every decoder is present." This suite answers that at CI time instead of at
// user-report time.
//
// It also guards a real risk found during the audit: the Debug and Release trees
// ship DIFFERENT avcodec DLLs (19 MB vs 13.7 MB), so a capability verified in
// one configuration is not evidence for the other. The inventory prints what the
// loaded binary can actually decode.
//
// What is asserted:
//   - the decoders REX's default hardware path depends on are all present
//     (H.264 / HEVC / VP9 / AV1) — losing any of these breaks the product;
//   - the inventory is non-empty and deterministic (a build that silently lost
//     every decoder would fail here rather than at the user's desktop).

#include <gtest/gtest.h>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace {

std::vector<std::string> VideoDecoderNames() {
    std::vector<std::string> names;
    void* opaque = nullptr;
    const AVCodec* c = nullptr;
    while ((c = av_codec_iterate(&opaque)) != nullptr) {
        if (!av_codec_is_decoder(c)) continue;
        if (c->type != AVMEDIA_TYPE_VIDEO) continue;
        names.emplace_back(c->name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

bool CanDecode(AVCodecID id) {
    return avcodec_find_decoder(id) != nullptr;
}

}  // namespace

TEST(DecoderInventory, VideoDecodersArePresentInShippedBinary) {
    const auto names = VideoDecoderNames();
    for (const std::string& n : names) {
        // Printed so a CI log records exactly what this build can decode.
        // This is the evidence behind any "supports codec X" claim.
        RecordProperty("video_decoder", n);
    }
    std::printf("[DecoderInventory] %zu video decoders in shipped avcodec:\n",
                names.size());
    for (const std::string& n : names) std::printf("  %s\n", n.c_str());

    ASSERT_FALSE(names.empty())
        << "shipped avcodec exposes no video decoders at all";
}

// The four codecs the engine's product promise depends on. If the FFmpeg build
// ever changes and drops one, this fails immediately instead of at runtime.
TEST(DecoderInventory, CoreCodecsDecode) {
    EXPECT_TRUE(CanDecode(AV_CODEC_ID_H264))  << "H.264/AVC decoder missing";
    EXPECT_TRUE(CanDecode(AV_CODEC_ID_HEVC))  << "HEVC/H.265 decoder missing";
    EXPECT_TRUE(CanDecode(AV_CODEC_ID_VP9))   << "VP9 decoder missing";
    EXPECT_TRUE(CanDecode(AV_CODEC_ID_AV1))   << "AV1 decoder missing";
}

// Common secondary codecs. Informational-by-design: a missing one is a
// capability reduction, not a build break, so these are asserted only as a
// recorded fact the inventory test surfaces.
TEST(DecoderInventory, SecondaryCodecsRecorded) {
    struct Entry { AVCodecID id; const char* name; };
    static constexpr Entry kSecondary[] = {
        {AV_CODEC_ID_MPEG4,      "MPEG-4 Part 2"},
        {AV_CODEC_ID_MPEG2VIDEO, "MPEG-2"},
        {AV_CODEC_ID_VP8,        "VP8"},
        {AV_CODEC_ID_VC1,        "VC-1"},
        {AV_CODEC_ID_THEORA,     "Theora"},
        {AV_CODEC_ID_PRORES,     "ProRes"},
        {AV_CODEC_ID_DNXHD,      "DNxHD"},
        {AV_CODEC_ID_UTVIDEO,    "UTVideo"},
        {AV_CODEC_ID_FFV1,       "FFV1"},
        {AV_CODEC_ID_WMV3,       "WMV v3"},
        {AV_CODEC_ID_RV40,       "RealVideo 4.0"},
    };
    int present = 0;
    for (const Entry& e : kSecondary) {
        if (CanDecode(e.id)) {
            ++present;
            std::printf("[DecoderInventory] present: %s\n", e.name);
        } else {
            std::printf("[DecoderInventory] ABSENT : %s\n", e.name);
        }
    }
    // MPEG-4 Part 2 and MPEG-2 are the baseline container-era codecs; losing
    // all of the secondary set would indicate a broken build rather than a
    // trimmed one.
    EXPECT_GT(present, 0) << "no secondary video decoders present at all";
}