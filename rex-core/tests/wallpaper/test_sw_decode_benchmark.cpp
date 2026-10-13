// Software-decode threading benchmark.
//
// The roadmap deferred the thread_count change with: "I would defer the
// software-decoder thread-count change until it has its own benchmark." This is
// that benchmark. REX currently pins software decode to thread_count = 1
// (FfmpegVideoDecoder.cpp), which LAV/mpv/VLC/Chromium all avoid -- but they
// measure on their own machines, so the claim is unverified here.
//
// Design notes:
//   * It measures by DECODING, not by opening, because open is dominated by
//     file I/O and container parsing.
//   * thread_count is varied per run; the decoder is reopened so no state
//     leaks between measurements.
//   * This reports numbers. It deliberately does NOT assert which setting is
//     faster -- the measurement decides that, per hardware, and hard-coding an
//     expectation would make the suite lie on other machines.
//
// Set REX_BENCH_VIDEO to a clip (a 4K source is the interesting case). Without
// it the benchmark SKIPs rather than fails.

#include <gtest/gtest.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

std::string BenchClip() {
    const char* v = std::getenv("REX_BENCH_VIDEO");
    return v ? std::string(v) : std::string();
}

struct DecodeResult {
    int frames = 0;
    double wallSeconds = 0.0;
    double decodeMsTotal = 0.0;
    double fps() const {
        return wallSeconds > 0.0 ? static_cast<double>(frames) / wallSeconds : 0.0;
    }
    double msPerFrame() const {
        return frames > 0 ? decodeMsTotal / frames : 0.0;
    }
};

// Decode the whole clip in software at the requested thread count. Frames are
// discarded after decode, so this measures the decode pipeline only.
DecodeResult DecodeSoftware(const std::string& path, int threadCount,
                            int maxFrames) {
    DecodeResult r;
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
        return r;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        return r;
    }
    const int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vs < 0) {
        avformat_close_input(&fmt);
        return r;
    }
    const AVCodec* dec = avcodec_find_decoder(fmt->streams[vs]->codecpar->codec_id);
    if (!dec) {
        avformat_close_input(&fmt);
        return r;
    }
    AVCodecContext* c = avcodec_alloc_context3(dec);
    if (!c || avcodec_parameters_to_context(c, fmt->streams[vs]->codecpar) < 0) {
        avcodec_free_context(&c);
        avformat_close_input(&fmt);
        return r;
    }
    // 0 = let FFmpeg choose (av_cpu_count); 1 = the current REX setting.
    c->thread_count = threadCount;
    if (threadCount > 0) c->thread_type = 0;
    // Some containers (notably the AV1-in-MP4 case) leave codecpar->format
    // unset, and the decoder then fails with "Failed to get pixel format".
    // The hardware path sees NV12 for this content, so YUV420P is the matching
    // software format.
    if (c->pix_fmt == AV_PIX_FMT_NONE) c->pix_fmt = AV_PIX_FMT_YUV420P;
    if (avcodec_open2(c, dec, nullptr) < 0) {
        avcodec_free_context(&c);
        avformat_close_input(&fmt);
        return r;
    }

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    const auto t0 = std::chrono::steady_clock::now();
    bool eof = false;
    while (r.frames < maxFrames) {
        const int rc = avcodec_receive_frame(c, frame);
        if (rc == 0) {
            ++r.frames;
            av_frame_unref(frame);
            const double ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - t0).count();
            r.decodeMsTotal = ms;
            continue;
        }
        if (rc != AVERROR(EAGAIN) && rc != AVERROR_EOF) break;
        if (eof) break;
        const int rr = av_read_frame(fmt, pkt);
        if (rr == AVERROR_EOF) {
            avcodec_send_packet(c, nullptr);
            eof = true;
            continue;
        }
        if (rr < 0) break;
        if (pkt->stream_index == vs) {
            const int s = avcodec_send_packet(c, pkt);
            if (s != 0 && s != AVERROR(EAGAIN)) break;
        }
        av_packet_unref(pkt);
    }
    r.wallSeconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();

    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&c);
    avformat_close_input(&fmt);
    return r;
}

}  // namespace

TEST(SoftwareDecodeBenchmark, SingleThreadVsAutoThreads) {
    const std::string clip = BenchClip();
    if (clip.empty()) {
        GTEST_SKIP() << "set REX_BENCH_VIDEO to a clip to run this benchmark";
    }
    // Enough frames to average out container seek noise but stay quick.
    const int kMaxFrames = 300;

    const DecodeResult one = DecodeSoftware(clip, /*threadCount=*/1, kMaxFrames);
    const DecodeResult auto_ = DecodeSoftware(clip, /*threadCount=*/0, kMaxFrames);

    ASSERT_GT(one.frames, 0) << "single-threaded decode produced nothing";
    ASSERT_GT(auto_.frames, 0) << "auto-threaded decode produced nothing";

    std::printf(
        "[SoftwareDecodeBenchmark] %s\n"
        "  thread_count=1 : %d frames in %.2fs => %.1f fps, %.2f ms/frame\n"
        "  thread_count=0 : %d frames in %.2fs => %.1f fps, %.2f ms/frame\n"
        "  speedup (auto / single) = %.2fx\n",
        clip.c_str(), one.frames, one.wallSeconds, one.fps(), one.msPerFrame(),
        auto_.frames, auto_.wallSeconds, auto_.fps(), auto_.msPerFrame(),
        auto_.fps() / (one.fps() > 0 ? one.fps() : 1.0));

    // Sanity, not a performance expectation: both settings must produce the
    // same number of frames. A difference means threading changed output, which
    // would be a correctness bug regardless of speed.
    EXPECT_EQ(one.frames, auto_.frames)
        << "thread count changed the decoded frame count";
}

TEST(SoftwareDecodeBenchmark, AutoThreadsAreActuallyConfigured) {
    const std::string clip = BenchClip();
    if (clip.empty()) {
        GTEST_SKIP() << "set REX_BENCH_VIDEO to a clip to run this benchmark";
    }
    // Sanity check that FFmpeg really expands thread_count=0, so the benchmark
    // above really is comparing 1 thread against N and not 1 against 1.
    const AVCodec* dec = avcodec_find_decoder(AV_CODEC_ID_H264);
    ASSERT_NE(dec, nullptr);
    AVCodecContext* c = avcodec_alloc_context3(dec);
    ASSERT_NE(c, nullptr);
    c->thread_count = 0;
    c->width = 1920;
    c->height = 1080;
    c->pix_fmt = AV_PIX_FMT_YUV420P;
    if (avcodec_open2(c, dec, nullptr) == 0) {
        std::printf("[SoftwareDecodeBenchmark] thread_count=0 expanded to %d\n",
                    c->thread_count);
        EXPECT_GT(c->thread_count, 1)
            << "FFmpeg did not expand thread_count=0; the A/B is meaningless";
        avcodec_free_context(&c);
    } else {
        avcodec_free_context(&c);
        GTEST_SKIP() << "could not open a reference H.264 context";
    }
}