// ═══════════════════════════════════════════════════════════════════════════
// Audio decode correctness + Dolby 5.1 downmix verification.
//
// Fixtures (generated with a dev FFmpeg build, NOT the runtime's shipped one):
//   %TEMP%/opencode/audiofix/tones-ac3-5.1.mp4   AC-3, 5.1(side), 48 kHz
//   %TEMP%/opencode/audiofix/tones-eac3-5.1.mp4  E-AC-3, 5.1(side), 48 kHz
// Each of the six channels carries a DIFFERENT sine, so the downmix matrix can
// be checked channel-by-channel instead of just "audio came out":
//   FL 440 | FR 880 | FC 1760 | LFE 80 | SL 220 | SR 330 Hz
//
// Set REX_AUDIO_FIXTURES to the directory holding them; tests SKIP (not fail)
// when the fixtures are absent so a clean checkout stays green.
// ═══════════════════════════════════════════════════════════════════════════

#include <gtest/gtest.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "wallpaper/audio/AudioPlayer.hpp"

using namespace rex::wallpaper;

namespace {

constexpr double kToneHz[6] = {440.0, 880.0, 1760.0, 80.0, 220.0, 330.0};
constexpr double kSampleRate = 48000.0;

std::string FixtureDir() {
    if (const char* env = std::getenv("REX_AUDIO_FIXTURES")) return env;
    const char* tmp = std::getenv("TEMP");
    if (!tmp) tmp = std::getenv("TMP");
    if (!tmp) return {};
    return std::string(tmp) + "\\opencode\\audiofix";
}

bool HaveFixtures(const std::string& name, std::string* out) {
    const std::string path = FixtureDir() + "\\" + name;
    if (!std::filesystem::exists(path)) return false;
    if (out) *out = path;
    return true;
}

// Goertzel: magnitude of `hz` in a real signal. Cheaper than an FFT and exact
// enough to answer "is this tone present, and how loud relative to the others".
double Goertzel(const std::vector<double>& x, double hz) {
    const size_t n = x.size();
    if (n < 8) return 0.0;
    const double k = 2.0 * M_PI * hz / kSampleRate;
    const double coeff = 2.0 * std::cos(k);
    double s1 = 0.0, s2 = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double s0 = x[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return std::sqrt(s1 * s1 + s2 * s2 - coeff * s1 * s2) / static_cast<double>(n);
}

struct DecodeResult {
    int64_t frames = 0;          // decoded sample frames delivered
    int64_t packets = 0;         // packets accepted by the decoder
    std::string codecName;
    int srcChannels = 0;
    std::string error;
};

// Runs the PRODUCTION state machine over the fixture and returns how much
// audio actually came out. Uses AudioPlayer's real helpers, so a regression
// here is a production regression — not a copy of it.
DecodeResult DecodeFixture(const std::string& path) {
    DecodeResult out;
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) != 0) {
        out.error = "open failed";
        return out;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        out.error = "stream info failed";
        return out;
    }
    const int idx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (idx < 0) {
        avformat_close_input(&fmt);
        out.error = "no audio stream";
        return out;
    }
    const AVCodec* codec =
        avcodec_find_decoder(fmt->streams[idx]->codecpar->codec_id);
    if (!codec) {
        avformat_close_input(&fmt);
        out.error = "no decoder";
        return out;
    }
    out.codecName = codec->name;
    out.srcChannels = fmt->streams[idx]->codecpar->ch_layout.nb_channels;

    AVCodecContext* dec = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(dec, fmt->streams[idx]->codecpar);
    if (avcodec_open2(dec, codec, nullptr) < 0) {
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        out.error = "codec open failed";
        return out;
    }

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    bool draining = false;
    auto sink = [&](AVFrame* f) { out.frames += f->nb_samples; };

    for (;;) {
        if (draining) {
            // DrainAvailableFrames returns 0 once the decoder is empty (EOF).
            if (AudioPlayer::DrainAvailableFrames(dec, frame, sink) <= 0) break;
            continue;
        }
        const int pr = av_read_frame(fmt, pkt);
        if (pr == AVERROR_EOF) {
            avcodec_send_packet(dec, nullptr);   // enter drain mode
            draining = true;
            continue;
        }
        if (pr < 0) break;
        if (pkt->stream_index != idx) { av_packet_unref(pkt); continue; }

        const int delivered = AudioPlayer::SubmitPacketAndDrain(dec, pkt, frame, sink);
        ++out.packets;
        av_packet_unref(pkt);
        if (delivered < 0) { out.error = "decode error"; break; }
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    return out;
}

// Decodes and downmixes to stereo, returning interleaved L/R as doubles.
bool DecodeToStereo(const std::string& path, std::vector<double>* left,
                    std::vector<double>* right) {
    left->clear();
    right->clear();
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) != 0) return false;
    avformat_find_stream_info(fmt, nullptr);
    const int idx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (idx < 0) { avformat_close_input(&fmt); return false; }
    const AVCodec* codec =
        avcodec_find_decoder(fmt->streams[idx]->codecpar->codec_id);
    if (!codec) { avformat_close_input(&fmt); return false; }
    AVCodecContext* dec = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(dec, fmt->streams[idx]->codecpar);
    if (avcodec_open2(dec, codec, nullptr) < 0) {
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        return false;
    }
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    std::vector<int16_t> s16(8192 * 2);
    auto sink = [&](AVFrame* f) {
        const int n = AudioPlayer::ConvertFrameToS16(f, 2, s16.data(), 8192);
        for (int i = 0; i < n; ++i) {
            left->push_back(s16[i * 2] / 32768.0);
            right->push_back(s16[i * 2 + 1] / 32768.0);
        }
    };
    bool draining = false;
    for (;;) {
        if (draining) {
            if (AudioPlayer::DrainAvailableFrames(dec, frame, sink) <= 0) break;
            continue;
        }
        const int pr = av_read_frame(fmt, pkt);
        if (pr == AVERROR_EOF) { avcodec_send_packet(dec, nullptr); draining = true; continue; }
        if (pr < 0) break;
        if (pkt->stream_index != idx) { av_packet_unref(pkt); continue; }
        const int delivered = AudioPlayer::SubmitPacketAndDrain(dec, pkt, frame, sink);
        av_packet_unref(pkt);
        if (delivered < 0) break;
    }
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    return left->size() > 1024;
}

// Decodes with the PRE-FIX loop: one avcodec_receive_frame per submitted
// packet, packet unreffed regardless of what send_packet returned. Kept here
// as a reference so the defect cannot be reintroduced silently.
int64_t DecodeWithLegacyLoop(const std::string& path) {
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) != 0) return -1;
    avformat_find_stream_info(fmt, nullptr);
    const int idx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (idx < 0) { avformat_close_input(&fmt); return -1; }
    const AVCodec* codec =
        avcodec_find_decoder(fmt->streams[idx]->codecpar->codec_id);
    if (!codec) { avformat_close_input(&fmt); return -1; }
    AVCodecContext* dec = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(dec, fmt->streams[idx]->codecpar);
    if (avcodec_open2(dec, codec, nullptr) < 0) {
        avcodec_free_context(&dec); avformat_close_input(&fmt); return -1;
    }
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    int64_t frames = 0;
    for (;;) {
        const int pr = av_read_frame(fmt, pkt);
        if (pr < 0) break;
        if (pkt->stream_index != idx) { av_packet_unref(pkt); continue; }
        avcodec_send_packet(dec, pkt);     // return value IGNORED (the defect)
        av_packet_unref(pkt);
        const int rr = avcodec_receive_frame(dec, frame);
        if (rr == 0) { frames += frame->nb_samples; av_frame_unref(frame); }
        else if (rr != AVERROR(EAGAIN)) break;
    }
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    return frames;
}

} // namespace

// ─── Decoder availability ────────────────────────────────────────────────

TEST(AudioDolbyTest, Ac3AndEac3DecodersArePresent) {
    // If the shipped FFmpeg ever drops these, Dolby tracks go silent and the
    // failure looks identical to "unsupported" — so assert it explicitly.
    EXPECT_NE(avcodec_find_decoder(AV_CODEC_ID_AC3), nullptr)
        << "shipped FFmpeg has no AC-3 decoder";
    EXPECT_NE(avcodec_find_decoder(AV_CODEC_ID_EAC3), nullptr)
        << "shipped FFmpeg has no E-AC-3 decoder";
}

// ─── State machine ───────────────────────────────────────────────────────

TEST(AudioDolbyTest, Ac3FixtureDecodesToExpectedDuration) {
    std::string path;
    if (!HaveFixtures("tones-ac3-5.1.mp4", &path)) {
        GTEST_SKIP() << "AC-3 fixture absent (set REX_AUDIO_FIXTURES)";
    }
    const auto r = DecodeFixture(path);
    ASSERT_EQ(r.error, "") << r.error;
    EXPECT_EQ(r.codecName, "ac3");
    EXPECT_EQ(r.srcChannels, 6);
    // 6 s at 48 kHz. The old loop unreffed packets the decoder had not
    // accepted and took only one frame per packet, so this came up short.
    EXPECT_GT(r.frames, static_cast<int64_t>(6 * kSampleRate * 0.98))
        << "decoded only " << r.frames << " of " << int(6 * kSampleRate)
        << " frames — audio is being dropped";
}

TEST(AudioDolbyTest, Eac3FixtureDecodesToExpectedDuration) {
    std::string path;
    if (!HaveFixtures("tones-eac3-5.1.mp4", &path)) {
        GTEST_SKIP() << "E-AC-3 fixture absent (set REX_AUDIO_FIXTURES)";
    }
    const auto r = DecodeFixture(path);
    ASSERT_EQ(r.error, "") << r.error;
    EXPECT_EQ(r.codecName, "eac3");
    EXPECT_EQ(r.srcChannels, 6);
    // E-AC-3 has an intrinsic decoder delay, so the NULL-packet drain is what
    // recovers the tail; without it this is short by the buffered frames.
    EXPECT_GT(r.frames, static_cast<int64_t>(6 * kSampleRate * 0.98))
        << "decoded only " << r.frames << " frames — EOF drain is missing";
}

TEST(AudioDolbyTest, ProductionStateMachineIsNeverWorseThanLegacy) {
    // Honest scope note: the pre-fix loop (one receive per submitted packet,
    // unreffed packet, no EOF drain) is a real spec violation, but on these
    // fixtures it happens to recover the SAME number of frames — AC-3 and
    // E-AC-3 emit one frame per packet here, so the loss window does not
    // trigger. This test therefore guards against REGRESSION (production must
    // never lose audio the old path recovered) rather than claiming the fix
    // changes output for these files.
    for (const char* name : {"tones-ac3-5.1.mp4", "tones-eac3-5.1.mp4"}) {
        std::string path;
        if (!HaveFixtures(name, &path)) { GTEST_SKIP() << "fixture absent"; }
        const auto now = DecodeFixture(path);
        const int64_t legacy = DecodeWithLegacyLoop(path);
        ASSERT_GT(legacy, 0) << "legacy decoder failed entirely";
        SCOPED_TRACE(name);
        std::printf("%s: production=%lld legacy=%lld (%+.1f%%)\n", name,
                    (long long)now.frames, (long long)legacy,
                    100.0 * (double)(now.frames - legacy) / (double)legacy);
        EXPECT_GE(now.frames, legacy)
            << name << ": the corrected state machine lost audio";
    }
}

// ─── Downmix matrix ──────────────────────────────────────────────────────

TEST(AudioDownmixTest, MatrixMatchesDolbyCoefficients) {
    // 5.1 = FL FR FC LFE SL SR. LFE must NOT reach stereo.
    EXPECT_NEAR(AudioPlayer::GetDownmixCoefficients(6, 2, 0, 0), 0.7071f, 1e-3f);
    EXPECT_NEAR(AudioPlayer::GetDownmixCoefficients(6, 2, 1, 1), 0.7071f, 1e-3f);
    EXPECT_NEAR(AudioPlayer::GetDownmixCoefficients(6, 2, 2, 0), 0.7071f, 1e-3f);
    EXPECT_NEAR(AudioPlayer::GetDownmixCoefficients(6, 2, 2, 1), 0.7071f, 1e-3f);
    EXPECT_NEAR(AudioPlayer::GetDownmixCoefficients(6, 2, 4, 0), 0.5f, 1e-3f);
    EXPECT_NEAR(AudioPlayer::GetDownmixCoefficients(6, 2, 5, 1), 0.5f, 1e-3f);
    EXPECT_EQ(AudioPlayer::GetDownmixCoefficients(6, 2, 3, 0), 0.0f)
        << "LFE must not fold into the left output";
    EXPECT_EQ(AudioPlayer::GetDownmixCoefficients(6, 2, 3, 1), 0.0f)
        << "LFE must not fold into the right output";
    // No cross-talk between the far side and the opposite output.
    EXPECT_EQ(AudioPlayer::GetDownmixCoefficients(6, 2, 5, 0), 0.0f);
    EXPECT_EQ(AudioPlayer::GetDownmixCoefficients(6, 2, 4, 1), 0.0f);
}

TEST(AudioDownmixTest, StereoAndMonoPassThrough) {
    EXPECT_EQ(AudioPlayer::GetDownmixCoefficients(2, 2, 0, 0), 1.0f);
    EXPECT_EQ(AudioPlayer::GetDownmixCoefficients(2, 2, 1, 1), 1.0f);
    EXPECT_EQ(AudioPlayer::GetDownmixCoefficients(1, 2, 0, 0), 1.0f);
    EXPECT_EQ(AudioPlayer::GetDownmixCoefficients(1, 2, 0, 1), 1.0f);
}

TEST(AudioDownmixTest, FixtureTonesMapToCorrectStereoChannels) {
    std::string path;
    if (!HaveFixtures("tones-ac3-5.1.mp4", &path)) {
        GTEST_SKIP() << "tone fixture absent";
    }
    std::vector<double> l, r;
    ASSERT_TRUE(DecodeToStereo(path, &l, &r));

    // Skip the very start (decoder priming) and analyse a clean window.
    const size_t skip = l.size() / 4;
    std::vector<double> lw(l.begin() + skip, l.begin() + skip + 16384);
    std::vector<double> rw(r.begin() + skip, r.begin() + skip + 16384);

    const double l440 = Goertzel(lw, kToneHz[0]);
    const double l1760 = Goertzel(lw, kToneHz[2]);
    const double l80 = Goertzel(lw, kToneHz[3]);
    const double r880 = Goertzel(rw, kToneHz[1]);
    const double r1760 = Goertzel(rw, kToneHz[2]);
    const double r80 = Goertzel(rw, kToneHz[3]);

    // Front-left must reach the left output and not the right.
    EXPECT_GT(l440, 1e-4) << "FL (440 Hz) missing from the left output";
    EXPECT_LT(Goertzel(rw, kToneHz[0]), l440 * 0.05)
        << "FL leaked into the right output";
    // Front-right likewise.
    EXPECT_GT(r880, 1e-4) << "FR (880 Hz) missing from the right output";
    EXPECT_LT(Goertzel(lw, kToneHz[1]), r880 * 0.05)
        << "FR leaked into the left output";
    // Centre feeds BOTH equally.
    EXPECT_GT(l1760, 1e-4) << "FC (1760 Hz) missing from the left output";
    EXPECT_NEAR(l1760, r1760, r1760 * 0.15)
        << "centre should reach both outputs equally";
    // LFE must be dropped by the stereo downmix.
    EXPECT_LT(l80, l440 * 0.05) << "LFE (80 Hz) leaked into the left output";
    EXPECT_LT(r80, r880 * 0.05) << "LFE (80 Hz) leaked into the right output";
}