// Send/receive contract tests for the video decode loop.
//
// The defect these cover: FfmpegVideoDecoder::DecodeFrame used to
// av_packet_unref() the packet and treat AVERROR(EAGAIN) from
// avcodec_send_packet() as success. Per the FFmpeg contract an EAGAIN means the
// packet was NOT consumed, so unreffing it permanently destroyed a compressed
// packet — a dropped frame plus a discontinuity fed to the decoder, which is
// intermittent-stutter shaped rather than a constant rate deficit.
//
// The suite builds a real compressed stream in memory (no fixture files), then
// drives the same send/receive structure the decoder uses, so the assertions are
// about FFmpeg's actual behaviour rather than a mock.

#include <gtest/gtest.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
}

#include "wallpaper/decoder/FfmpegVideoDecoder.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

// Minimal big-endian-free MP4-in-memory carrier is overkill here: MPEG-TS needs
// a muxer, so instead we encode raw H.264 Annex-B and feed the decoder directly.
// That exercises exactly the avcodec_send_packet/receive_frame contract under
// test without depending on a container.
constexpr int kWidth = 64;
constexpr int kHeight = 64;
constexpr int kFrames = 12;

// Splits an Annex-B H.264 stream on 3-byte start codes and reports the NAL unit
// type of each (byte 0 of the NAL header). Types 1 (non-IDR slice) and 5 (IDR
// slice) are VCL and each decodes to exactly one frame; 6/7/8/9 are parameter
// sets / access-unit delimiters and decode to nothing.
struct NalUnit {
    size_t start;
    size_t end;
    int type;
};

std::vector<NalUnit> SplitAnnexB(const std::vector<uint8_t>& b) {
    std::vector<std::pair<size_t, size_t>> spans;
    for (size_t i = 0; i + 3 < b.size();) {
        if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1) {
            spans.emplace_back(i, i + 3);
            i += 3;
        } else {
            if (!spans.empty()) spans.back().second = i + 3;
            ++i;
        }
    }
    if (!spans.empty()) spans.back().second = b.size();

    std::vector<NalUnit> out;
    out.reserve(spans.size());
    for (auto& [s, e] : spans) {
        const size_t hdr = s + 3;
        const int type = hdr < e ? (b[hdr] & 0x1F) : 0;
        out.push_back({s, e, type});
    }
    return out;
}

int64_t CountVclNals(const std::vector<uint8_t>& b) {
    int64_t n = 0;
    for (const NalUnit& u : SplitAnnexB(b)) {
        if (u.type == 1 || u.type == 5) ++n;
    }
    return n;
}

struct EncodedStream {
    std::vector<uint8_t> bytes;
    int64_t frameCount = 0;
    AVCodecID codecId = AV_CODEC_ID_NONE;
};

// The shipped runtime DLL ships decoders only — no video encoders are compiled
// in. Probe the candidates and use whichever this build actually has, so the
// suite adapts instead of failing on a codec that is absent by design.
AVCodecID PickVideoCodec() {
    static constexpr AVCodecID kCandidates[] = {
        AV_CODEC_ID_H264,  AV_CODEC_ID_MPEG4,   AV_CODEC_ID_MJPEG,
        AV_CODEC_ID_FFV1,  AV_CODEC_ID_MPEG2VIDEO, AV_CODEC_ID_MPEG1VIDEO,
        AV_CODEC_ID_VP9,   AV_CODEC_ID_H263,
    };
    for (AVCodecID id : kCandidates) {
        if (avcodec_find_encoder(id)) return id;
    }
    return AV_CODEC_ID_NONE;
}

EncodedStream MakeTestStream() {
    EncodedStream out;
    out.codecId = PickVideoCodec();
    if (out.codecId == AV_CODEC_ID_NONE) return out;

    const AVCodec* enc = avcodec_find_encoder(out.codecId);
    if (!enc) return out;

    AVCodecContext* c = avcodec_alloc_context3(enc);
    if (!c) return out;
    c->width = kWidth;
    c->height = kHeight;
    // Every candidate wants a planar YUV pixel format.
    c->pix_fmt = AV_PIX_FMT_YUV420P;
    c->time_base = AVRational{1, 25};
    c->framerate = AVRational{25, 1};
    c->gop_size = 1;          // every frame a keyframe: no B-frame reordering
    c->max_b_frames = 0;
    c->bit_rate = 400000;

    if (avcodec_open2(c, enc, nullptr) < 0) {
        avcodec_free_context(&c);
        out.codecId = AV_CODEC_ID_NONE;
        return out;
    }

    AVFrame* frame = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    av_frame_get_buffer(frame, 32);
    frame->format = c->pix_fmt;
    frame->width = c->width;
    frame->height = c->height;

    for (int i = 0; i < kFrames; ++i) {
        av_frame_make_writable(frame);
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                frame->data[0][y * frame->linesize[0] + x] =
                    static_cast<uint8_t>((x + y + i * 8) & 0xFF);
            }
        }
        for (int y = 0; y < kHeight / 2; ++y) {
            for (int x = 0; x < kWidth / 2; ++x) {
                frame->data[1][y * frame->linesize[1] + x] =
                    static_cast<uint8_t>((128 + i * 4) & 0xFF);
                frame->data[2][y * frame->linesize[2] + x] =
                    static_cast<uint8_t>((128 - i * 4) & 0xFF);
            }
        }
        frame->pts = i;

        if (avcodec_send_frame(c, frame) < 0) break;
        while (avcodec_receive_packet(c, pkt) == 0) {
            out.bytes.insert(out.bytes.end(), pkt->data, pkt->data + pkt->size);
            av_packet_unref(pkt);
            ++out.frameCount;
        }
    }
    avcodec_send_frame(c, nullptr);
    while (avcodec_receive_packet(c, pkt) == 0) {
        out.bytes.insert(out.bytes.end(), pkt->data, pkt->data + pkt->size);
        av_packet_unref(pkt);
        ++out.frameCount;
    }

    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&c);
    return out;
}

// A stand-in for FfmpegVideoDecoder's packet pump: one packet at a time, with
// the SAME retry structure (retry the same packet on send-EAGAIN instead of
// dropping it). `maxSendBudget` lets a test clamp the retry budget to prove the
// bounded path terminates.
struct DecodeOutcome {
    int framesOut = 0;
    int64_t lastPts = AV_NOPTS_VALUE;
    int sendRetries = 0;
    int droppedPackets = 0;
    bool monotonicPts = true;
};

DecodeOutcome RunDecodeLoop(const std::vector<uint8_t>& bytes,
                            AVCodecContext* dec, int maxSendBudget) {
    DecodeOutcome r;

// Slice the Annex-B stream back into packets on start codes so each
    // avcodec_send_packet carries exactly one NAL, as the real demuxer does.
    for (const NalUnit& nal : SplitAnnexB(bytes)) {
        AVPacket* p = av_packet_alloc();
        const int size = static_cast<int>(nal.end - nal.start);
p->data = static_cast<uint8_t*>(av_malloc(size));
        std::memcpy(p->data, bytes.data() + nal.start, size);
        p->size = size;

        int retries = 0;
        while (true) {
            const int s = avcodec_send_packet(dec, p);
            if (s == AVERROR(EAGAIN)) {
                // The contract: the packet was not consumed. Drain output,
                // then retry the SAME packet. The helper consumes the retry
                // and resets the count once spent (see its header comment).
                if (rex::wallpaper::ShouldRetrySendPacket(retries, maxSendBudget)) {
                    ++r.sendRetries;
                    AVFrame* drain = av_frame_alloc();
                    while (avcodec_receive_frame(dec, drain) == 0) {
                        ++r.framesOut;
                        if (r.lastPts != AV_NOPTS_VALUE &&
                            drain->pts <= r.lastPts) {
                            r.monotonicPts = false;
                        }
                        r.lastPts = drain->pts;
                        av_frame_unref(drain);
                    }
                    av_frame_free(&drain);
                    continue;
                }
                ++r.droppedPackets;  // budget exhausted: give up on it
            }
            break;
        }
        av_packet_free(&p);
    }

    avcodec_send_packet(dec, nullptr);
    AVFrame* drain = av_frame_alloc();
    while (avcodec_receive_frame(dec, drain) == 0) {
        ++r.framesOut;
        if (r.lastPts != AV_NOPTS_VALUE && drain->pts <= r.lastPts) {
            r.monotonicPts = false;
        }
        r.lastPts = drain->pts;
        av_frame_unref(drain);
    }
    av_frame_free(&drain);
    return r;
}

AVCodecContext* MakeDecoderFor(const EncodedStream& s) {
    if (s.codecId == AV_CODEC_ID_NONE) return nullptr;
    const AVCodec* dec = avcodec_find_decoder(s.codecId);
    if (!dec || s.bytes.empty()) return nullptr;
    AVCodecContext* c = avcodec_alloc_context3(dec);
    if (!c) return nullptr;
    c->thread_count = 1;
    if (avcodec_open2(c, dec, nullptr) < 0) {
        avcodec_free_context(&c);
        return nullptr;
    }
    return c;
}

// This build ships decoders but NO video encoders, so the stream is normally
// loaded from a checked-in 12 KB H.264 fixture. Set REX_PACKET_FIXTURE to
// override the path. The in-memory encoder stays as a fallback for builds that
// do have an encoder.
std::string FixturePath() {
    if (const char* env = std::getenv("REX_PACKET_FIXTURE")) return env;
    // __FILE__ is the absolute source path recorded at compile time, so the
    // fixture is found without depending on the working directory.
    std::string self(__FILE__);
    const size_t slash = self.find_last_of("/\\");
    if (slash == std::string::npos) return {};
    self.resize(slash);                       // …/tests/wallpaper
    const size_t up = self.find_last_of("/\\");
    if (up == std::string::npos) return {};
    self.resize(up);                          // …/tests
    return self + "/fixtures/packet_contract.h264";
}

EncodedStream LoadFixtureStream() {
    EncodedStream out;
    const std::string path = FixturePath();
    if (path.empty()) return out;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n > 0) {
        out.bytes.resize(static_cast<size_t>(n));
        const size_t got = std::fread(out.bytes.data(), 1,
                                      out.bytes.size(), f);
        out.bytes.resize(got);
    }
    std::fclose(f);
    if (!out.bytes.empty()) {
        out.codecId = AV_CODEC_ID_H264;
        // Expected output = number of VCL NALs. An Annex-B stream also carries
        // SPS/PPS/SEI NALs, which decode to no frame; counting them would make
        // the conservation assertion compare against the wrong number.
        out.frameCount = CountVclNals(out.bytes);
    }
    return out;
}

const EncodedStream& Stream() {
    static const EncodedStream fixture = LoadFixtureStream();
    if (fixture.bytes.empty()) {
        static const EncodedStream encoded = MakeTestStream();
        return encoded;
    }
    return fixture;
}

// This build ships no video encoder, so the round-trip tests can only run where
// one exists. SKIP rather than fail — an absent encoder is a property of the
// build, not a contract violation.
#define REX_REQUIRE_VIDEO_ENCODER()                                        \
    if (Stream().codecId == AV_CODEC_ID_NONE || Stream().bytes.empty()) {   \
        GTEST_SKIP() << "no video encoder in this build; cannot build a "  \
                        "stream to decode";                                \
    }

} // namespace

// ─── Retry policy ────────────────────────────────────────────────────────

TEST(SendPacketContract, RetriesWhileUnderBudget) {
    using rex::wallpaper::ShouldRetrySendPacket;
    int retries = 0;
    // The helper CONSUMES one retry per call: eight calls spend the budget.
    for (int i = 0; i < 8; ++i) EXPECT_TRUE(ShouldRetrySendPacket(retries, 8));
    // Budget exhausted -> the decoder was refusing input; advance the demuxer.
    // (The next call RE-ARMS the budget for the next packet; that contract is
    // asserted in ExhaustedBudgetResetsForTheNextPacket.)
    EXPECT_FALSE(ShouldRetrySendPacket(retries, 8));
}

TEST(SendPacketContract, ZeroBudgetNeverRetries) {
    using rex::wallpaper::ShouldRetrySendPacket;
    int retries = 0;
    EXPECT_FALSE(ShouldRetrySendPacket(retries, 0));
}

TEST(SendPacketContract, ExhaustedBudgetResetsForTheNextPacket) {
    // A spent budget must bound THIS packet only. The defect this guards: the
    // decoder kept its exhausted retry count across packets, so after ONE
    // saturated episode every later EAGAIN packet was dropped instantly with
    // no retry at all — decode holes while the decoder saturates (exactly
    // when the startup refill burst loads it), i.e. the intermittent
    // start-stutter signature this suite exists to pin down.
    using rex::wallpaper::ShouldRetrySendPacket;
    int retries = 0;
    for (int i = 0; i < 8; ++i) ASSERT_TRUE(ShouldRetrySendPacket(retries, 8));
    EXPECT_FALSE(ShouldRetrySendPacket(retries, 8))
        << "the packet must be dropped once the budget is spent";
    EXPECT_EQ(retries, 0)
        << "the spent budget must reset so the NEXT packet gets a fresh one";
    EXPECT_TRUE(ShouldRetrySendPacket(retries, 8))
        << "the next packet's first EAGAIN must retry again";
}

// ─── Round trip ──────────────────────────────────────────────────────────

TEST(SendPacketContract, EncodedStreamIsUsable) {
    REX_REQUIRE_VIDEO_ENCODER();
    ASSERT_GT(Stream().bytes.size(), 0u);
    EXPECT_GT(Stream().frameCount, 0);
}

TEST(SendPacketContract, DecodesEveryFrameWithMonotonicPts) {
    REX_REQUIRE_VIDEO_ENCODER();
    AVCodecContext* dec = MakeDecoderFor(Stream());
    ASSERT_NE(dec, nullptr);
    const auto r = RunDecodeLoop(Stream().bytes, dec, /*maxSendBudget=*/8);
    avcodec_free_context(&dec);

    // Frame conservation: no packet may be silently destroyed.
    EXPECT_EQ(r.framesOut, Stream().frameCount)
        << "decoded " << r.framesOut << " of " << Stream().frameCount
        << " — a packet was dropped";
    EXPECT_EQ(r.droppedPackets, 0) << "retry budget exhausted in real playback";
    EXPECT_TRUE(r.monotonicPts) << "PTS went backwards; decoder saw a discontinuity";
}

TEST(SendPacketContract, BoundedRetryTerminatesAndNeverExceedsFrameCount) {
    AVCodecContext* dec = MakeDecoderFor(Stream());
    ASSERT_NE(dec, nullptr);
    // Budget of 0 exercises the "give up on this packet on the first EAGAIN"
    // branch. The point of this test is that the loop TERMINATES (an unbounded
    // retry spins forever holding the decode mutex — proven live) and never
    // emits more frames than were encoded. It does not assert conservation:
    // deliberately dropping packets is lossy by construction, and non-VCL NALs
    // (SPS/PPS) can be dropped too, so framesOut+dropped legitimately exceeds
    // the VCL count when the budget is 0.
    const auto r = RunDecodeLoop(Stream().bytes, dec, /*maxSendBudget=*/0);
    avcodec_free_context(&dec);

    EXPECT_LE(r.framesOut, Stream().frameCount)
        << "emitted more frames than were encoded";
    // Reaching here at all proves no hang.
}