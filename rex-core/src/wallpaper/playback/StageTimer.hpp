#pragma once

// ─── Per-frame stage timing (diagnostic) ────────────────────────────────
//
// Records per-frame pipeline stage durations into small fixed rings for
// offline deadline analysis ("which stage misses the budget"): demux /
// send / receive+convert on the decode thread, begin-wait / draw-submit /
// end-frame(present) on the render thread. Rings are overwritten in place
// (latest N survive); dumps to %TEMP%\Paiint\perframe_*.csv happen only when
// REX_PERFRAME_CSV=1, so steady-state cost when off is a few integer ops.
//
// Conventions:
//   - All durations are integer microseconds (steady-clock deltas).
//   - arrivalUs = wall gap since the previous PRODUCED frame on the same
//     thread (decode) or since the previous loop pass (present). First
//     sample of a run is 0.
//   - Units/format of the CSV are fixed by StageCsvHeader(); analysis code
//     must not guess column order.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rex::wallpaper {

// One produced video frame, decode-thread stages.
struct DecodeStage {
    uint64_t seq = 0;          // ring sequence (join key within this file)
    int64_t frameIndex = 0;    // decoder frame index
    int64_t demuxUs = 0;       // av_read_frame accumulated this call
    int64_t sendUs = 0;        // avcodec_send_packet accumulated this call
    int64_t recvUs = 0;        // avcodec_receive_frame accumulated this call
    int64_t convUs = 0;        // ConvertAndSignal (incl. VP-Blt when taken)
    int64_t arrivalUs = 0;     // wall gap since previous produced frame
    int64_t queueDepth = 0;    // scheduler pending count at publish (stamped
                               // by the controller; -1 when unknown)
};

// One render-loop pass that reached EndFrame, render-thread stages.
struct PresentStage {
    uint64_t seq = 0;             // ring sequence (join key within this file)
    uint64_t frameCount = 0;     // engine loop pass number
    int64_t beginWaitMs_x1000 = 0;  // frame-latency waitable block, ms*1000
    int64_t drawMs_x1000 = 0;    // DrawVideoFrame+RenderFrame CPU, ms*1000
    int64_t endMs_x1000 = 0;     // EndFrame CPU incl. Present(), ms*1000
    int64_t gapMs_x1000 = 0;     // submit-side present interval, ms*1000
    int64_t targetMs_x1000 = 0;  // content cadence target, ms*1000
    int dwell = 0;               // DXGI present-refresh count (0 = unknown)
    int presentedNew = 0;        // 1 = new video frame, 0 = cached re-present
    int deadlineMiss = 0;        // 1 when gapMs > 1.5 * targetMs
};

// Fixed overwriting ring: Push is O(1); Snapshot returns oldest-first.
template <typename T, size_t N>
class StageRing {
public:
    void Push(const T& s) {
        m_buf[m_next % N] = s;
        ++m_next;
    }
    // Number of valid entries (<= N).
    size_t Size() const { return m_next < N ? m_next : N; }
    // Oldest-first snapshot (empty when nothing pushed).
    std::vector<T> Snapshot() const {
        std::vector<T> out;
        const size_t n = Size();
        out.reserve(n);
        const size_t start = m_next < N ? 0 : m_next % N;
        for (size_t i = 0; i < n; ++i) out.push_back(m_buf[(start + i) % N]);
        return out;
    }
    void Clear() {
        m_next = 0;
    }
    // Most recently pushed sample, or nullptr when empty. The pointer is
    // only valid until the next Push (same-thread use).
    T* Back() {
        if (m_next == 0) return nullptr;
        return &m_buf[(m_next - 1) % N];
    }

private:
    std::array<T, N> m_buf{};
    size_t m_next = 0;
};

// Percentile over a copy (linear interpolation, same convention as the
// telemetry percentile rings). Values are doubles; empty => 0.0.
inline double StagePercentile(std::vector<double> v, double q) {
    if (v.empty()) return 0.0;
    if (q < 0.0) q = 0.0;
    if (q > 1.0) q = 1.0;
    for (size_t i = 1; i < v.size(); ++i) {
        const double key = v[i];
        size_t j = i;
        while (j > 0 && v[j - 1] > key) {
            v[j] = v[j - 1];
            --j;
        }
        v[j] = key;
    }
    const double pos = q * (static_cast<double>(v.size()) - 1.0);
    const size_t lo = static_cast<size_t>(pos);
    const size_t hi = lo + 1 < v.size() ? lo + 1 : lo;
    const double frac = pos - static_cast<double>(lo);
    return v[lo] + frac * (v[hi] - v[lo]);
}

inline const char* DecodeStageCsvHeader() {
    return "seq,frameIndex,demuxUs,sendUs,recvUs,convUs,arrivalUs,queueDepth";
}

inline std::string DecodeStageCsvRow(const DecodeStage& s) {
    return std::to_string(s.seq) + "," + std::to_string(s.frameIndex) + "," +
           std::to_string(s.demuxUs) + "," + std::to_string(s.sendUs) + "," +
           std::to_string(s.recvUs) + "," + std::to_string(s.convUs) + "," +
           std::to_string(s.arrivalUs) + "," + std::to_string(s.queueDepth);
}

inline const char* PresentStageCsvHeader() {
    return "seq,frameCount,beginWaitMs_x1000,drawMs_x1000,endMs_x1000,"
           "gapMs_x1000,targetMs_x1000,dwell,presentedNew,deadlineMiss";
}

inline std::string PresentStageCsvRow(const PresentStage& s) {
    return std::to_string(s.seq) + "," + std::to_string(s.frameCount) + "," +
           std::to_string(s.beginWaitMs_x1000) + "," +
           std::to_string(s.drawMs_x1000) + "," +
           std::to_string(s.endMs_x1000) + "," +
           std::to_string(s.gapMs_x1000) + "," +
           std::to_string(s.targetMs_x1000) + "," + std::to_string(s.dwell) +
           "," + std::to_string(s.presentedNew) + "," +
           std::to_string(s.deadlineMiss);
}

}  // namespace rex::wallpaper
