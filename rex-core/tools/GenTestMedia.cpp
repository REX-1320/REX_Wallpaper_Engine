// GenTestMedia — dev tool: writes a short synthetic MP4 (MPEG4 video with a
// moving gradient + AAC sine-wave audio) so the runtime / preview surface can
// be exercised without shipping media. Usage: rex-gen-media [out.mp4]

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
}

static constexpr int kWidth = 320;
static constexpr int kHeight = 240;
static constexpr int kFps = 30;
static constexpr int kSeconds = 4;
static constexpr int kSampleRate = 44100;

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : "test.mp4";

    AVFormatContext* fmt = nullptr;
    if (avformat_alloc_output_context2(&fmt, nullptr, "mp4", path.c_str()) < 0) {
        std::fprintf(stderr, "cannot create mp4 muxer\n");
        return 1;
    }
    if (!(fmt->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&fmt->pb, path.c_str(), AVIO_FLAG_WRITE) < 0) {
            std::fprintf(stderr, "cannot open output file\n");
            return 1;
        }
    }

    // ─── Video stream (MPEG4, yuv420p) ───────────────────────────────────
    const AVCodec* vcodec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    if (!vcodec) {
        std::fprintf(stderr, "mpeg4 encoder unavailable\n");
        return 1;
    }
    AVCodecContext* vctx = avcodec_alloc_context3(vcodec);
    vctx->width = kWidth;
    vctx->height = kHeight;
    vctx->time_base = AVRational{1, kFps};
    vctx->framerate = AVRational{kFps, 1};
    vctx->pix_fmt = AV_PIX_FMT_YUV420P;
    vctx->bit_rate = 800000;
    vctx->gop_size = 12;
    vctx->max_b_frames = 0;
    if (avcodec_open2(vctx, vcodec, nullptr) < 0) {
        std::fprintf(stderr, "cannot open mpeg4 encoder\n");
        return 1;
    }
    AVStream* vstream = avformat_new_stream(fmt, vcodec);
    vstream->time_base = vctx->time_base;
    avcodec_parameters_from_context(vstream->codecpar, vctx);

    // ─── Audio stream (AAC, stereo fltp) ──────────────────────────────────
    // Optional: the vcpkg FFmpeg build may ship without a native AAC encoder.
    const bool noAudio = argc > 2 && std::string(argv[2]) == "--no-audio";
    const AVCodec* acodec = noAudio ? nullptr : avcodec_find_encoder(AV_CODEC_ID_AAC);
    AVCodecContext* actx = nullptr;
    AVStream* astream = nullptr;
    if (!acodec) {
        std::fprintf(stderr, "aac encoder unavailable; writing video-only file\n");
    } else {
        actx = avcodec_alloc_context3(acodec);
        actx->sample_rate = kSampleRate;
        actx->sample_fmt = AV_SAMPLE_FMT_FLTP;
        av_channel_layout_default(&actx->ch_layout, 2);
        actx->bit_rate = 96000;
        if (avcodec_open2(actx, acodec, nullptr) < 0) {
            std::fprintf(stderr, "cannot open aac encoder; writing video-only file\n");
            avcodec_free_context(&actx);
            actx = nullptr;
        } else {
            astream = avformat_new_stream(fmt, acodec);
            if (!astream) {
                std::fprintf(stderr, "avformat_new_stream(audio) failed\n");
                return 1;
            }
            astream->time_base = AVRational{1, kSampleRate};
            avcodec_parameters_from_context(astream->codecpar, actx);
        }
    }

    if (avformat_write_header(fmt, nullptr) < 0) {
        std::fprintf(stderr, "cannot write header\n");
        return 1;
    }

    // ─── Encode loop ──────────────────────────────────────────────────────
    AVFrame* vframe = av_frame_alloc();
    vframe->format = vctx->pix_fmt;
    vframe->width = vctx->width;
    vframe->height = vctx->height;
    av_frame_get_buffer(vframe, 32);

    AVPacket* pkt = av_packet_alloc();
    const int totalFrames = kSeconds * kFps;

    for (int i = 0; i < totalFrames; ++i) {
        // Moving gradient: color changes every frame (FRAME_READY detection).
        av_frame_make_writable(vframe);
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                const int t = i;
                vframe->data[0][y * vframe->linesize[0] + x] =
                    static_cast<uint8_t>((x * 255 / kWidth + t * 6) & 0xFF);
            }
        }
        for (int y = 0; y < kHeight / 2; ++y) {
            std::memset(vframe->data[1] + y * vframe->linesize[1], 128, kWidth / 2);
            std::memset(vframe->data[2] + y * vframe->linesize[2], 128, kWidth / 2);
        }
        vframe->pts = i;
        if (avcodec_send_frame(vctx, vframe) == 0) {
            while (avcodec_receive_packet(vctx, pkt) == 0) {
                pkt->stream_index = vstream->index;
                av_packet_rescale_ts(pkt, vctx->time_base, vstream->time_base);
                av_interleaved_write_frame(fmt, pkt);
                av_packet_unref(pkt);
            }
        }
    }
    if (avcodec_send_frame(vctx, nullptr) == 0) {
        while (avcodec_receive_packet(vctx, pkt) == 0) {
            pkt->stream_index = vstream->index;
            av_packet_rescale_ts(pkt, vctx->time_base, vstream->time_base);
            av_interleaved_write_frame(fmt, pkt);
            av_packet_unref(pkt);
        }
    }

    // Audio: 440 Hz sine in 1024-sample chunks (the AAC frame size; only
    // when an audio stream was created above).
    if (astream) {
        AVFrame* aframe = av_frame_alloc();
        aframe->format = actx->sample_fmt;
        aframe->ch_layout = actx->ch_layout;
        aframe->sample_rate = actx->sample_rate;
        aframe->nb_samples = 1024;
        av_frame_get_buffer(aframe, 0);
        const int totalChunks = (kSeconds * kSampleRate + 1023) / 1024;
        for (int i = 0; i < totalChunks; ++i) {
            av_frame_make_writable(aframe);
            {
                float* l = reinterpret_cast<float*>(aframe->data[0]);
                float* r = reinterpret_cast<float*>(aframe->data[1]);
                for (int n = 0; n < 1024; ++n) {
                    const double t = (static_cast<double>(i * 1024 + n) / kSampleRate) * 2.0 * 3.14159265 * 440.0;
                    const float s = static_cast<float>(0.2 * std::sin(t));
                    l[n] = s;
                    r[n] = s;
                }
            }
            aframe->pts = static_cast<int64_t>(i) * 1024;
            if (avcodec_send_frame(actx, aframe) == 0) {
                while (avcodec_receive_packet(actx, pkt) == 0) {
                    pkt->stream_index = astream->index;
                    av_packet_rescale_ts(pkt, actx->time_base, astream->time_base);
                    av_interleaved_write_frame(fmt, pkt);
                    av_packet_unref(pkt);
                }
            }
        }
        if (avcodec_send_frame(actx, nullptr) == 0) {
            while (avcodec_receive_packet(actx, pkt) == 0) {
                pkt->stream_index = astream->index;
                av_packet_rescale_ts(pkt, actx->time_base, astream->time_base);
                av_interleaved_write_frame(fmt, pkt);
                av_packet_unref(pkt);
            }
        }
        av_frame_free(&aframe);
    }

    av_write_trailer(fmt);
    std::printf("wrote %s (%d video frames%s)\n", path.c_str(), totalFrames,
                astream ? ", with audio" : "");

    if (actx) {
        avcodec_free_context(&actx);
    }
    av_frame_free(&vframe);
    av_packet_free(&pkt);
    avcodec_free_context(&vctx);
    avformat_free_context(fmt);
    return 0;
}
