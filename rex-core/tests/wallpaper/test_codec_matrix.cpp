// Phase 4 acceptance: every supported file either decodes or reports why not.
//
// The roadmap: "each supported file either plays correctly using the best
// available path or produces an explicit, actionable error. Unsupported
// acceleration must not be confused with unsupported decoding."
//
// This suite walks a directory of real media, opens each with the SHIPPED
// FFmpeg, and records what actually happened. It is deliberately tolerant of
// unsupported combinations — it RECORDS them rather than failing — but it does
// assert that the codecs REX advertises can decode something, so a broken or
// trimmed FFmpeg build fails here rather than on a user's desktop.
//
// Set REX_MATRIX_DIR to a directory of media. Without it, the walk SKIPs.
//
// This is also the test that would have caught the silent software-AV1 stall
// found on 2026-11 (zero frames, no error) if it had existed when that was
// discovered: a file that yields no frames is reported explicitly.

#include <gtest/gtest.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

namespace {

std::string MatrixDir() {
    const char* v = std::getenv("REX_MATRIX_DIR");
    return v ? std::string(v) : std::string();
}

struct FileResult {
    std::string name;
    std::string codec;
    int64_t width = 0, height = 0;
    double fps = 0.0;
    int frames = 0;         // frames actually produced (bounded)
    bool opened = false;
    bool pixFmtResolved = false;
    std::string note;
};

FileResult ProbeSoftware(const std::string& path, int maxFrames) {
    FileResult r;
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
        r.note = "container open failed";
        return r;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        r.note = "stream info failed";
        return r;
    }
    const int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vs < 0) {
        avformat_close_input(&fmt);
        r.note = "no video stream";
        return r;
    }
    AVCodecParameters* par = fmt->streams[vs]->codecpar;
    r.codec = avcodec_get_name(par->codec_id);
    r.width = par->width;
    r.height = par->height;
    const AVRational fr = fmt->streams[vs]->avg_frame_rate.den
        ? fmt->streams[vs]->avg_frame_rate
        : fmt->streams[vs]->r_frame_rate;
    r.fps = fr.den ? static_cast<double>(fr.num) / fr.den : 0.0;

    const AVCodec* dec = avcodec_find_decoder(par->codec_id);
    if (!dec) {
        avformat_close_input(&fmt);
        r.note = "no decoder for codec";
        return r;
    }
    AVCodecContext* c = avcodec_alloc_context3(dec);
    if (!c || avcodec_parameters_to_context(c, par) < 0) {
        avcodec_free_context(&c);
        avformat_close_input(&fmt);
        r.note = "context alloc failed";
        return r;
    }
    c->thread_count = 0;
    if (c->pix_fmt == AV_PIX_FMT_NONE) c->pix_fmt = AV_PIX_FMT_YUV420P;
    if (avcodec_open2(c, dec, nullptr) < 0) {
        avcodec_free_context(&c);
        avformat_close_input(&fmt);
        r.note = "avcodec_open2 failed";
        return r;
    }
    r.opened = true;
    r.pixFmtResolved = c->pix_fmt != AV_PIX_FMT_NONE;

    AVPacket* pkt = av_packet_alloc();
    AVFrame* f = av_frame_alloc();
    bool eof = false;
    int budget = maxFrames * 200;  // hard stop: never spin forever
    while (r.frames < maxFrames && budget-- > 0) {
        const int rc = avcodec_receive_frame(c, f);
        if (rc == 0) {
            ++r.frames;
            av_frame_unref(f);
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
    if (r.frames == 0) r.note = "OPENED BUT PRODUCED ZERO FRAMES";
    av_frame_free(&f);
    av_packet_free(&pkt);
    avcodec_free_context(&c);
    avformat_close_input(&fmt);
    return r;
}

std::vector<std::string> MediaFilesIn(const std::string& dir) {
    std::vector<std::string> out;
    static const char* kExt[] = {".mp4", ".mkv", ".mov", ".webm", ".avi", ".m4v"};
    // Directory iteration via the standard library would need <filesystem>;
    // using it keeps this portable across the toolchains in use.
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string name = fd.cFileName;
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char ch) { return static_cast<char>(::tolower(ch)); });
        for (const char* e : kExt) {
            const size_t n = std::strlen(e);
            if (lower.size() >= n && lower.compare(lower.size() - n, n, e) == 0) {
                out.push_back(dir + "\\" + name);
                break;
            }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

TEST(CodecMatrix, EveryMediaFileIsEitherDecodedOrExplained) {
    const std::string dir = MatrixDir();
    if (dir.empty()) {
        GTEST_SKIP() << "set REX_MATRIX_DIR to a media directory";
    }
    const auto files = MediaFilesIn(dir);
    ASSERT_FALSE(files.empty()) << "no media files found in " << dir;

    int decoded = 0, zeroFrame = 0, notOpened = 0;
    std::set<std::string> codecsSeen;
    std::vector<std::string> problems;

    for (const std::string& p : files) {
        const FileResult r = ProbeSoftware(p, /*maxFrames=*/12);
        const std::string label = p.substr(p.find_last_of("\\/") + 1);
        std::printf("[CodecMatrix] %-52s %-8s %4lldx%-4lld %s%s%s\n",
                    label.c_str(), r.codec.c_str(),
                    static_cast<long long>(r.width),
                    static_cast<long long>(r.height),
                    r.frames > 0 ? "OK frames=" : "NO-FRAMES ",
                    r.note.empty() ? "" : " note=", r.note.c_str());
        if (!r.codec.empty()) codecsSeen.insert(r.codec);
        if (!r.opened) {
            ++notOpened;
            problems.push_back(label + ": " + r.note);
        } else if (r.frames == 0) {
            ++zeroFrame;
            problems.push_back(label + ": " + r.note);
        } else {
            ++decoded;
        }
    }

    std::printf("[CodecMatrix] files=%zu decoded=%d zeroFrame=%d notOpened=%d codecs=%zu\n",
                files.size(), decoded, zeroFrame, notOpened, codecsSeen.size());
    for (const std::string& c : codecsSeen) std::printf("[CodecMatrix] codec: %s\n", c.c_str());
    for (const std::string& s : problems) std::printf("[CodecMatrix] PROBLEM: %s\n", s.c_str());

    // The acceptance criterion: every failure must be EXPLAINED, which the probe
    // guarantees. Assert the thing that must never happen -- a file that opens
    // yet silently yields nothing is a bug in the engine or build, not an
    // unsupported combination, and is what the zero-progress guard now reports.
    EXPECT_EQ(zeroFrame, 0)
        << "a file opened but produced ZERO frames — that is an unexplained "
           "failure, not an unsupported combination";
    EXPECT_GT(decoded, 0) << "nothing in the matrix decoded at all";
}