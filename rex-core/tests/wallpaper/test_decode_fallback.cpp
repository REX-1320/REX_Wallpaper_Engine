// DecodePathUnusable predicate tests (P2-1 latch, re-landed stepwise).
//
// Zero frames decoded after `budget` DecodeFrame attempts (or a send-fail
// streak on a never-decoded path) means the path is unusable, not slow:
// surface it so the supervisor can poison-park the file instead of
// stalling on it forever. Attempts reset on the first decoded frame, so a
// slow-but-working path never trips this.
//
// NOTE: the per-codec GUID probe + its tests re-land separately (Step 2)
// with string-parsed constants; this file covers only the latch predicate.

#include <gtest/gtest.h>

#include "wallpaper/decoder/FfmpegVideoDecoder.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <d3d11_4.h>

#include "wallpaper/decoder/FfmpegVideoDecoder.hpp"

using rex::wallpaper::CheckHwProfileSupport;
using rex::wallpaper::D3d11vaCanDecodeChroma;
using rex::wallpaper::DecodePathUnusable;
using rex::wallpaper::HwProfileSupport;
using rex::wallpaper::PlanSwsSlices;
using rex::wallpaper::SignalSharedFenceOrdered;
using rex::wallpaper::SliceSourcePointers;
using rex::wallpaper::SwsCacheHit;
using rex::wallpaper::SwsCacheKey;
using rex::wallpaper::SwsColorParams;
using rex::wallpaper::SwsColorParamsFor;
using rex::wallpaper::SwsParallelStripsAllowed;
using rex::wallpaper::VideoColorInfo;

namespace {

// Parses a canonical "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX" GUID string.
// The strings below are quoted verbatim against FFmpeg libavcodec/dxva2.c
// (ff_DXVA2_Mode*), mingw-w64 dxva2api.idl, Microsoft Learn "Direct3D 12
// video GUIDs", and Firefox DXVA2Manager.cpp — this is what makes the tests
// independent of the table under test. 2026-10-16: the table shipped with
// fabricated Data4 tails built by hand-rolled byte helpers like the old
// MakeGuid here; parsing spec strings is the structural fix.
GUID GuidFromString(const char* s) {
  GUID g{};
  unsigned int d4[8];
  const int n =
      std::sscanf(s, "%08lX-%04hX-%04hX-%02X%02X-%02X%02X%02X%02X%02X%02X",
                  &g.Data1, &g.Data2, &g.Data3, &d4[0], &d4[1], &d4[2],
                  &d4[3], &d4[4], &d4[5], &d4[6], &d4[7]);
  EXPECT_EQ(n, 11) << "bad GUID literal in test: " << s;
  for (int i = 0; i < 8; ++i) g.Data4[i] = static_cast<unsigned char>(d4[i]);
  return g;
}

// DXVA2_ModeH264_VLD_NoFGT (empirically proven too: H.264 opens on hardware).
const GUID kH264Vld =
    GuidFromString("1b81be68-a0c7-11d3-b984-00c04f2e73c5");
// DXVA2_ModeHEVC_VLD_Main10.
const GUID kHevcMain10 =
    GuidFromString("107af0e0-ef1a-4d19-aba8-67a163073d13");

}  // namespace

TEST(HwProfileProbe, H264GuidMeansH264Supported) {
  EXPECT_EQ(CheckHwProfileSupport(AV_CODEC_ID_H264, &kH264Vld, 1),
            HwProfileSupport::Supported);
}

TEST(HwProfileProbe, H264GuidMeansAv1Unsupported) {
  EXPECT_EQ(CheckHwProfileSupport(AV_CODEC_ID_AV1, &kH264Vld, 1),
            HwProfileSupport::Unsupported);
}

TEST(HwProfileProbe, EmptyListIsUnsupportedForKnownCodecs) {
  EXPECT_EQ(CheckHwProfileSupport(AV_CODEC_ID_H264, nullptr, 0),
            HwProfileSupport::Unsupported);
}

TEST(HwProfileProbe, UnknownCodecAttemptsHardware) {
  const GUID list[] = {kH264Vld, kHevcMain10};
  EXPECT_EQ(CheckHwProfileSupport(AV_CODEC_ID_MPEG4, list, 2),
            HwProfileSupport::UnknownCodec);
}

TEST(HwProfileProbe, AnyFamilyGuidMatches) {
  EXPECT_EQ(CheckHwProfileSupport(AV_CODEC_ID_HEVC, &kHevcMain10, 1),
            HwProfileSupport::Supported);
  EXPECT_EQ(CheckHwProfileSupport(AV_CODEC_ID_H264, &kHevcMain10, 1),
            HwProfileSupport::Unsupported);
}

TEST(HwProfileProbe, EveryTableEntryMatchesItsCanonicalString) {
  // Pins each shipped constant against its spec string: a single wrong
  // nibble anywhere in the table fails here instead of silently forcing
  // software decode with wrong colors (2026-10-16).
  struct Case {
    const char* guid;
    AVCodecID codec;
  };
  const Case cases[] = {
      {"1b81be68-a0c7-11d3-b984-00c04f2e73c5", AV_CODEC_ID_H264},
      {"5b11d51b-2f4c-4452-bcc3-09f2a1160cc0", AV_CODEC_ID_HEVC},
      {"107af0e0-ef1a-4d19-aba8-67a163073d13", AV_CODEC_ID_HEVC},
      {"463707f8-a1d0-4585-876d-83aa6d60b89e", AV_CODEC_ID_VP9},
      {"a4c749ef-6ecf-48aa-8448-50a7a1165ff7", AV_CODEC_ID_VP9},
      {"b8be4ccb-cf53-46ba-8d59-d6b8a6da5d2a", AV_CODEC_ID_AV1},
      {"ee27417f-5e77-4d14-a53b-7dc2ed571ff0", AV_CODEC_ID_MPEG2VIDEO},
      {"1b81be94-a0c7-11d3-b984-00c04f2e73c5", AV_CODEC_ID_VC1},
  };
  for (const auto& c : cases) {
    const GUID g = GuidFromString(c.guid);
    EXPECT_EQ(CheckHwProfileSupport(c.codec, &g, 1),
              HwProfileSupport::Supported)
        << "table mismatch for " << c.guid;
  }
  const GUID hevc = GuidFromString("5b11d51b-2f4c-4452-bcc3-09f2a1160cc0");
  EXPECT_EQ(CheckHwProfileSupport(AV_CODEC_ID_AV1, &hevc, 1),
            HwProfileSupport::Unsupported);
}

TEST(DecodePathUnusable, ZeroFramesPastBudgetIsUnusable) {
  EXPECT_TRUE(DecodePathUnusable(0, 240, 240));
  EXPECT_TRUE(DecodePathUnusable(0, 1000, 240));
}

TEST(DecodePathUnusable, SlowButWorkingNeverTrips) {
  EXPECT_FALSE(DecodePathUnusable(0, 239, 240));
  EXPECT_FALSE(DecodePathUnusable(1, 10000, 240));
  EXPECT_FALSE(DecodePathUnusable(5, 0, 240));
}

TEST(DecodePathUnusable, CustomBudget) {
  EXPECT_TRUE(DecodePathUnusable(0, 8, 8));
  EXPECT_FALSE(DecodePathUnusable(0, 7, 8));
}

// ─── D3D11VA chroma gate (2026-10-17) ───────────────────────────────────
//
// The Oppenheimer Rext 4:4:4 clip opens with hwaccel attached, then FFmpeg
// falls back to software decode INSIDE the stream while keeping the
// hardware path's single-thread pin — measured 92-121 ms/frame (~8-11 fps)
// vs 21 ms/frame with 8 threads. D3D11VA only ever produces 4:2:0
// surfaces; skipping the doomed attempt up front keeps software decode on
// auto threads.

TEST(HwChromaGate, FourTwentyFormatsAttemptHardware) {
  EXPECT_TRUE(D3d11vaCanDecodeChroma(AV_PIX_FMT_YUV420P));
  EXPECT_TRUE(D3d11vaCanDecodeChroma(AV_PIX_FMT_YUV420P10LE));
  EXPECT_TRUE(D3d11vaCanDecodeChroma(AV_PIX_FMT_YUV420P12LE));
  EXPECT_TRUE(D3d11vaCanDecodeChroma(AV_PIX_FMT_NV12));
  EXPECT_TRUE(D3d11vaCanDecodeChroma(AV_PIX_FMT_P010LE));
}

TEST(HwChromaGate, NonFourTwentyRefused) {
  EXPECT_FALSE(D3d11vaCanDecodeChroma(AV_PIX_FMT_YUV444P10LE));  // Rext clip
  EXPECT_FALSE(D3d11vaCanDecodeChroma(AV_PIX_FMT_YUV444P));
  EXPECT_FALSE(D3d11vaCanDecodeChroma(AV_PIX_FMT_YUV422P));
  EXPECT_FALSE(D3d11vaCanDecodeChroma(AV_PIX_FMT_GRAY8));
}

TEST(HwChromaGate, UnknownFormatKeepsStatusQuo) {
  // AV_PIX_FMT_NONE means "not resolved yet": attempt hardware, never
  // refuse on uncertainty (same rule as HwProfileSupport::UnknownCodec).
  EXPECT_TRUE(D3d11vaCanDecodeChroma(AV_PIX_FMT_NONE));
}

// ─── swscale colorimetry policy (2026-10-17) ────────────────────────────
//
// Every SwsContext creation must apply the source matrix/range; contexts
// left on swscale defaults convert BT.709 sources with the BT.601 matrix
// (deadpool tint, 2026-10-16). The cache key carries matrix and range so a
// context created before colorimetry was known can never be reused once it
// is known.

TEST(SwsColorPolicy, KnownMatricesMapToSwsCoefficients) {
  const auto p709 = SwsColorParamsFor(VideoColorInfo::Matrix::BT709, false);
  EXPECT_EQ(p709.srcCsp, SWS_CS_ITU709);
  EXPECT_EQ(p709.srcRange, 0);
  EXPECT_EQ(p709.dstCsp, SWS_CS_ITU709);
  EXPECT_EQ(p709.dstRange, 1);

  const auto p601 = SwsColorParamsFor(VideoColorInfo::Matrix::BT601, false);
  EXPECT_EQ(p601.srcCsp, SWS_CS_SMPTE170M);
  EXPECT_EQ(p601.dstCsp, SWS_CS_SMPTE170M);

  const auto p2020 = SwsColorParamsFor(VideoColorInfo::Matrix::BT2020, true);
  EXPECT_EQ(p2020.srcCsp, SWS_CS_BT2020);
  EXPECT_EQ(p2020.srcRange, 1);  // full-range source flips src range only
  EXPECT_EQ(p2020.dstRange, 1);  // destination is always full-range RGB
}

TEST(SwsColorPolicy, FullRangeSourceFlipsSrcRangeOnly) {
  const auto p = SwsColorParamsFor(VideoColorInfo::Matrix::BT709, true);
  EXPECT_EQ(p.srcRange, 1);
  EXPECT_EQ(p.dstCsp, SWS_CS_ITU709);
  EXPECT_EQ(p.dstRange, 1);
}

TEST(SwsColorPolicy, CacheHitRequiresIdenticalColorimetryAndSize) {
  const SwsCacheKey want{AV_PIX_FMT_YUV444P10LE, false, SWS_CS_ITU709, 0, 3840,
                         1752};
  EXPECT_TRUE(SwsCacheHit(want, want));

  // A context created with different (or default 601) colorimetry must
  // never be reused for a 709 source.
  EXPECT_FALSE(SwsCacheHit(
      {AV_PIX_FMT_YUV444P10LE, false, SWS_CS_SMPTE170M, 0, 3840, 1752}, want));
  EXPECT_FALSE(SwsCacheHit(
      {AV_PIX_FMT_YUV444P10LE, false, SWS_CS_ITU709, 1, 3840, 1752}, want));
  EXPECT_FALSE(SwsCacheHit(
      {AV_PIX_FMT_YUV444P10LE, true, SWS_CS_ITU709, 0, 3840, 1752}, want));
  EXPECT_FALSE(SwsCacheHit(
      {AV_PIX_FMT_YUV420P10LE, false, SWS_CS_ITU709, 0, 3840, 1752}, want));

  // Same format + colorimetry but a different resolution (stream switch)
  // must recreate too — swscale contexts hold fixed dimensions.
  EXPECT_FALSE(SwsCacheHit(
      {AV_PIX_FMT_YUV444P10LE, false, SWS_CS_ITU709, 0, 1920, 1080}, want));
}

TEST(SwsColorPolicy, DefaultKeyNeverHits) {
  // Default-constructed key == the state before any context was created.
  const SwsCacheKey want{AV_PIX_FMT_YUV444P10LE, false, SWS_CS_ITU709, 0, 3840,
                         1752};
  EXPECT_FALSE(SwsCacheHit(SwsCacheKey{}, want));
}

// ─── Parallel conversion strips (FIX B, 2026-10-18) ─────────────────────
//
// 4:4:4 10-bit → BGRA has no SIMD fast path in swscale (~37-47 ms/frame at
// 4K; the 24 fps budget is 41.7 ms). Conversion is split into horizontal
// strips, one SwsContext per strip, each converting its strip as if it
// were a whole frame. The split is only valid if strip conversion is
// bit-identical to full-frame conversion — asserted here on real swscale
// for 4:4:4 and 4:2:2 10-bit (the eligible formats; 4:2:0 measured NOT
// bit-exact at strip boundaries, hence SwsParallelStripsAllowed).

namespace {

// Deterministic 10-bit pattern plane (no RNG): gradient + modest texture.
std::vector<uint16_t> MakePatternPlane10(int w, int h, uint16_t base) {
  std::vector<uint16_t> plane(static_cast<size_t>(w) * h);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      plane[static_cast<size_t>(y) * w + x] = static_cast<uint16_t>(
          (base + static_cast<unsigned>(x) * 7 +
           static_cast<unsigned>(y) * 13 +
           (static_cast<unsigned>(x) * static_cast<unsigned>(y) & 63u)) &
          1023u);
    }
  }
  return plane;
}

SwsContext* MakeLaneContext(int w, int h, AVPixelFormat srcFmt) {
  SwsContext* c = sws_getContext(w, h, srcFmt, w, h, AV_PIX_FMT_BGRA,
                                 SWS_BILINEAR, nullptr, nullptr, nullptr);
  if (c) {
    sws_setColorspaceDetails(c, sws_getCoefficients(SWS_CS_ITU709), 0,
                             sws_getCoefficients(SWS_CS_ITU709), 1, 0, 1 << 16,
                             1 << 16);
  }
  return c;
}

}  // namespace

TEST(SwsSlicePlan, SplitsRemainderOverLastStrip) {
  const auto s = PlanSwsSlices(10, 3);  // 3 + 3 + 4
  ASSERT_EQ(s.size(), 3u);
  EXPECT_EQ(s[0].y, 0);
  EXPECT_EQ(s[0].rows, 3);
  EXPECT_EQ(s[1].y, 3);
  EXPECT_EQ(s[1].rows, 3);
  EXPECT_EQ(s[2].y, 6);
  EXPECT_EQ(s[2].rows, 4);
}

TEST(SwsParallelPolicy, VerticalChromaSubsamplingRefused) {
  // 4:2:0 10-bit strips are NOT bit-exact (measured 2026-10-18: the two
  // luma rows straddling every strip boundary deviate from full-frame);
  // those sources stay on swscale's fast single-context converters.
  EXPECT_FALSE(SwsParallelStripsAllowed(AV_PIX_FMT_YUV420P));
  EXPECT_FALSE(SwsParallelStripsAllowed(AV_PIX_FMT_YUV420P10LE));
  EXPECT_FALSE(SwsParallelStripsAllowed(AV_PIX_FMT_NV12));
  EXPECT_FALSE(SwsParallelStripsAllowed(AV_PIX_FMT_P010LE));
  EXPECT_FALSE(SwsParallelStripsAllowed(AV_PIX_FMT_NONE));

  // No vertical subsampling → strips bit-exact (see the parity tests).
  EXPECT_TRUE(SwsParallelStripsAllowed(AV_PIX_FMT_YUV444P10LE));
  EXPECT_TRUE(SwsParallelStripsAllowed(AV_PIX_FMT_YUV444P));
  EXPECT_TRUE(SwsParallelStripsAllowed(AV_PIX_FMT_YUV422P10LE));
  EXPECT_TRUE(SwsParallelStripsAllowed(AV_PIX_FMT_YUV422P));
  EXPECT_TRUE(SwsParallelStripsAllowed(AV_PIX_FMT_GBRP));
  EXPECT_TRUE(SwsParallelStripsAllowed(AV_PIX_FMT_BGRA));
}

TEST(SwsSlicePlan, CoversEveryRowExactlyOnce) {
  for (const int h : {1, 5, 64, 1080, 1752}) {
    for (const int lanes : {1, 2, 3, 8}) {
      const auto s = PlanSwsSlices(h, lanes);
      ASSERT_FALSE(s.empty());
      EXPECT_LE(static_cast<int>(s.size()), lanes);
      int next = 0;
      for (const auto& sl : s) {
        EXPECT_EQ(sl.y, next);
        EXPECT_GE(sl.rows, 1);
        next += sl.rows;
      }
      EXPECT_EQ(next, h);
    }
  }
}

TEST(SwsSliceParity, Sliced444p10ToBgraIsBitExact) {
  // 385 rows / 4 lanes → 96-row strips plus a 97-row tail.
  constexpr int kWidth = 640;
  constexpr int kHeight = 385;
  auto y = MakePatternPlane10(kWidth, kHeight, 64);
  auto u = MakePatternPlane10(kWidth, kHeight, 480);
  auto v = MakePatternPlane10(kWidth, kHeight, 900);
  uint8_t* src[4] = {reinterpret_cast<uint8_t*>(y.data()),
                     reinterpret_cast<uint8_t*>(u.data()),
                     reinterpret_cast<uint8_t*>(v.data()), nullptr};
  const int srcStride[4] = {kWidth * 2, kWidth * 2, kWidth * 2, 0};
  const int dstPitch = kWidth * 4;
  const int dstStride[4] = {dstPitch, 0, 0, 0};

  std::vector<uint8_t> full(static_cast<size_t>(dstPitch) * kHeight);
  {
    SwsContext* c = MakeLaneContext(kWidth, kHeight, AV_PIX_FMT_YUV444P10LE);
    ASSERT_NE(c, nullptr);
    uint8_t* d[4] = {full.data(), nullptr, nullptr, nullptr};
    ASSERT_GT(sws_scale(c, src, srcStride, 0, kHeight, d, dstStride), 0);
    sws_freeContext(c);
  }

  const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(AV_PIX_FMT_YUV444P10LE);
  const auto slices = PlanSwsSlices(kHeight, 4);  // 96 x3 + 97
  ASSERT_GE(slices.size(), 2u);
  // Lane contexts are created once and reused across rounds exactly like
  // the engine reuses them across frames; any per-round state carry-over
  // in swscale would surface as a round-2 mismatch.
  std::vector<SwsContext*> lanes;
  for (size_t i = 0; i < slices.size(); ++i) {
    SwsContext* c = MakeLaneContext(kWidth, kHeight, AV_PIX_FMT_YUV444P10LE);
    ASSERT_NE(c, nullptr);
    lanes.push_back(c);
  }
  for (int round = 0; round < 2; ++round) {
    std::vector<uint8_t> sliced(full.size(), 0xAB);  // poisons missed rows
    for (size_t i = 0; i < slices.size(); ++i) {
      const auto& sl = slices[i];
      uint8_t* sv[4] = {};
      SliceSourcePointers(src, srcStride, sl.y, desc, sv);
      uint8_t* dv[4] = {sliced.data() + static_cast<size_t>(sl.y) * dstPitch,
                        nullptr, nullptr, nullptr};
      // Strip-as-frame: pointers offset to the strip, srcSliceY stays 0.
      ASSERT_GT(
          sws_scale(lanes[i], sv, srcStride, 0, sl.rows, dv, dstStride), 0);
    }
    EXPECT_EQ(std::memcmp(full.data(), sliced.data(), full.size()), 0)
        << "round " << round;
  }
  for (SwsContext* c : lanes) {
    sws_freeContext(c);
  }
}

TEST(SwsSliceParity, Sliced422p10ToBgraIsBitExact) {
  // 385 rows / 4 lanes → 96-row strips plus a 97-row tail. No vertical
  // chroma subsampling: chroma planes advance one row per luma row.
  constexpr int kWidth = 640;
  constexpr int kHeight = 385;
  constexpr int kCw = kWidth / 2;
  auto y = MakePatternPlane10(kWidth, kHeight, 64);
  auto u = MakePatternPlane10(kCw, kHeight, 480);
  auto v = MakePatternPlane10(kCw, kHeight, 900);
  uint8_t* src[4] = {reinterpret_cast<uint8_t*>(y.data()),
                     reinterpret_cast<uint8_t*>(u.data()),
                     reinterpret_cast<uint8_t*>(v.data()), nullptr};
  const int srcStride[4] = {kWidth * 2, kCw * 2, kCw * 2, 0};
  const int dstPitch = kWidth * 4;
  const int dstStride[4] = {dstPitch, 0, 0, 0};

  std::vector<uint8_t> full(static_cast<size_t>(dstPitch) * kHeight);
  {
    SwsContext* c = MakeLaneContext(kWidth, kHeight, AV_PIX_FMT_YUV422P10LE);
    ASSERT_NE(c, nullptr);
    uint8_t* d[4] = {full.data(), nullptr, nullptr, nullptr};
    ASSERT_GT(sws_scale(c, src, srcStride, 0, kHeight, d, dstStride), 0);
    sws_freeContext(c);
  }

  const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(AV_PIX_FMT_YUV422P10LE);
  const auto slices = PlanSwsSlices(kHeight, 4);  // 96 x3 + 97
  ASSERT_GE(slices.size(), 2u);
  std::vector<SwsContext*> lanes;
  for (size_t i = 0; i < slices.size(); ++i) {
    SwsContext* c = MakeLaneContext(kWidth, kHeight, AV_PIX_FMT_YUV422P10LE);
    ASSERT_NE(c, nullptr);
    lanes.push_back(c);
  }
  for (int round = 0; round < 2; ++round) {
    std::vector<uint8_t> sliced(full.size(), 0xAB);
    for (size_t i = 0; i < slices.size(); ++i) {
      const auto& sl = slices[i];
      uint8_t* sv[4] = {};
      SliceSourcePointers(src, srcStride, sl.y, desc, sv);
      uint8_t* dv[4] = {sliced.data() + static_cast<size_t>(sl.y) * dstPitch,
                        nullptr, nullptr, nullptr};
      // Strip-as-frame: pointers offset to the strip, srcSliceY stays 0.
      ASSERT_GT(
          sws_scale(lanes[i], sv, srcStride, 0, sl.rows, dv, dstStride), 0);
    }
    EXPECT_EQ(std::memcmp(full.data(), sliced.data(), full.size()), 0)
        << "round " << round;
  }
  for (SwsContext* c : lanes) {
    sws_freeContext(c);
  }
}

// ─── Shared fence sequence (audit 2026-10-17 #5) ─────────────────────────

TEST(SharedFenceSequence, FreshSessionValueClearsEveryPriorSignal) {
  // Old defect: every decoder Open reset its private counter to 1 while the
  // shared fence object kept the previous session's high-water mark, so a
  // consumer waiting on a fresh session's early frames could be released by
  // stale values before the copies had executed. Values must therefore keep
  // climbing across close/open cycles.
  UINT64 highWater = 0;
  for (int i = 0; i < 40; ++i) {  // "session 1"
    highWater = SignalSharedFenceOrdered([](UINT64) {});
  }
  const UINT64 fresh = SignalSharedFenceOrdered([](UINT64) {});  // "Open #2"
  EXPECT_GT(fresh, highWater);
}

TEST(SharedFenceSequence, SignalEnqueueOrderMatchesValueOrderUnderContention) {
  // Two decoders (global + per-monitor) draw concurrently on one shared
  // context. The enqueue callbacks must observe strictly increasing values:
  // if a lower value could be enqueued after a higher one, the fence would
  // move backwards and leave in-between waits stranded.
  std::mutex recordMutex;
  std::vector<UINT64> order;
  auto enqueue = [&](UINT64 v) {
    std::lock_guard<std::mutex> g(recordMutex);
    order.push_back(v);
  };
  auto draw = [&](int n) {
    for (int i = 0; i < n; ++i) {
      SignalSharedFenceOrdered(enqueue);
    }
  };
  std::thread a(draw, 300);
  std::thread b(draw, 300);
  a.join();
  b.join();

  ASSERT_EQ(static_cast<size_t>(600), order.size());
  bool strictlyIncreasing = true;
  for (size_t i = 1; i < order.size() && strictlyIncreasing; ++i) {
    strictlyIncreasing = order[i - 1] < order[i];
  }
  EXPECT_TRUE(strictlyIncreasing);
}
