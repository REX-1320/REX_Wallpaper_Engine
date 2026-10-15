// Are the UNTAGGED files actually limited-range studio swing?
//
// 20 of 55 files carry no colour tags at all, and REX assumes limited range
// (16-235) for them. If a file was already range-expanded by its encoder,
// that assumption lifts blacks and washes midtones — the reported symptom.
//
// This measures the ACTUAL Y distribution of a real decoded frame and reports
// where black and white actually sit. Limited-range files peak at 16 and 235;
// full-range files reach 0 and 255.
//
// Set REX_RANGE_DIR to a media directory. Without it, tests SKIP.

#include <gtest/gtest.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

extern "C" {
}

namespace {

std::string RangeDir() {
    const char* v = std::getenv("REX_RANGE_DIR");
    return v ? std::string(v) : std::string();
}

bool FileExists(const std::string& p) {
    return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

struct RangeStats {
    std::string name;
    bool tagged = false;
    bool opened = false;
    bool gotFrame = false;
    int width = 0, height = 0;
    int lumaMin = 255, lumaMax = 0;
    int lumaP01 = 0, lumaP99 = 0;
    double lumaMean = 0.0;
    int below16 = 0;   // pixels darker than limited-range black
    int above235 = 0;  // pixels brighter than limited-range white
    long samples = 0;
};

RangeStats MeasureLuma(const std::string& path, int frameIndex) {
    RangeStats st;
    st.name = path.substr(path.find_last_of("\\/") + 1);
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) return st;
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        return st;
    }
    const int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vs < 0) { avformat_close_input(&fmt); return st; }
    AVCodecParameters* par = fmt->streams[vs]->codecpar;
    st.tagged = (par->color_range != AVCOL_RANGE_UNSPECIFIED) ||
                (par->color_trc != AVCOL_TRC_UNSPECIFIED);
    const AVCodec* dec = avcodec_find_decoder(par->codec_id);
    if (!dec) { avformat_close_input(&fmt); return st; }
    AVCodecContext* ctx = avcodec_alloc_context3(dec);
    if (!ctx || avcodec_parameters_to_context(ctx, par) < 0) {
        avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        return st;
    }
    ctx->thread_count = 0;
    if (ctx->pix_fmt == AV_PIX_FMT_NONE) ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    if (avcodec_open2(ctx, dec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        avformat_close_input(&fmt);
        return st;
    }
    st.opened = true;

    AVPacket* pkt = av_packet_alloc();
    AVFrame* f = av_frame_alloc();
    int idx = 0;
    while (idx <= frameIndex) {
        const int rc = avcodec_receive_frame(ctx, f);
        if (rc == 0) {
            if (idx == frameIndex) {
                st.gotFrame = true;
                st.width = f->width; st.height = f->height;
                const int bpp = av_get_bits_per_pixel(
                    av_pix_fmt_desc_get(static_cast<AVPixelFormat>(f->format)));
                if (bpp > 0) {
                    const int lw = f->linesize[0];
                    const uint8_t* Y = f->data[0];
                    std::vector<int> hist(256, 0);
                    long sum = 0;
                    // Sample every 4th row/col: enough for percentiles, fast.
                    for (int y = 0; y < f->height; y += 4) {
                        const uint8_t* row = Y + static_cast<long>(y) * lw;
                        for (int x = 0; x < f->width; x += 4) {
                            const int v = row[x];
                            hist[v]++; sum += v; st.samples++;
                            if (v < st.lumaMin) st.lumaMin = v;
                            if (v > st.lumaMax) st.lumaMax = v;
                            if (v < 16) st.below16++;
                            if (v > 235) st.above235++;
                        }
                    }
                    if (st.samples) {
                        st.lumaMean = static_cast<double>(sum) / st.samples;
                        long acc = 0;
                        for (int v = 0; v < 256; v++) {
                            acc += hist[v];
                            if (acc >= st.samples / 100) { st.lumaP01 = v; break; }
                        }
                        acc = 0;
                        for (int v = 255; v >= 0; v--) {
                            acc += hist[v];
                            if (acc >= st.samples / 100) { st.lumaP99 = v; break; }
                        }
                    }
                }
                av_frame_unref(f);
            }
            idx++;
            continue;
        }
        if (rc != AVERROR(EAGAIN) && rc != AVERROR_EOF) break;
        const int rr = av_read_frame(fmt, pkt);
        if (rr < 0) break;
        if (pkt->stream_index == vs) {
            const int s = avcodec_send_packet(ctx, pkt);
            if (s != 0 && s != AVERROR(EAGAIN)) break;
        }
        av_packet_unref(pkt);
    }
    av_frame_free(&f);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    return st;
}

}  // namespace

TEST(ColorRangeProbe, UntaggedFilesAreActuallyLimitedRange) {
    const std::string dir = RangeDir();
    if (dir.empty()) GTEST_SKIP() << "set REX_RANGE_DIR to a media directory";

    static const char* kNames[] = {
        "obanai-x-kaburamaru-demon-slayer-moewalls-com.mp4",
        "arcane-viktor.3840x2160.mp4",
        "miles-morales-walking-skyscraper-spiderman-moewalls-com.mp4",
        "ahri-the-nine-tailed-fox-league-of-legends-moewalls-com.mp4",
        "mona-gi.3840x2160.mp4",           // TAGGED control (BT.709 limited)
    };

    std::printf("[Range] %-52s %-7s %-11s %-11s %-7s %-7s\n",
                "file", "tagged", "Ymin", "Ymax", "P01", "P99");
    int limited = 0, fullish = 0, untagged = 0;
    for (const char* nm : kNames) {
        const std::string p = dir + "\\" + nm;
        if (!FileExists(p)) { std::printf("[Range] MISSING %s\n", nm); continue; }
        const RangeStats s = MeasureLuma(p, 12);
        if (!s.gotFrame) { std::printf("[Range] no frame  %s\n", nm); continue; }
        std::printf("[Range] %-52s %-7s %2d/%3d     %2d/%3d     %2d/%3d  mean=%5.1f <16:%d >235:%d\n",
                    s.name.c_str(), s.tagged ? "yes" : "NO", s.lumaP01, s.lumaMin,
                    s.lumaP99, s.lumaMax, s.lumaP01, s.lumaP99, s.lumaMean,
                    s.below16, s.above235);
        // A genuinely limited-range file never uses codes below ~16 or above
        // ~235 in bulk; an expanded/full file does.
        const bool isLimited = (s.below16 * 100 < s.samples) &&
                               (s.above235 * 100 < s.samples);
        if (!s.tagged) {
            ++untagged;
            if (isLimited) ++limited; else ++fullish;
        }
    }
    std::printf("[Range] untagged classified: limited=%d fullish=%d (total %d)\n",
                limited, fullish, untagged);
    // Reporting only: this test's job is to MEASURE and expose the truth,
    // not to assert a conclusion the engine has not been shown to need.
    // A future fix to the unspecified-range default keys off these numbers.
}
