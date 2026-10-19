#pragma once

// ─── Stdin/stdout frame protocol: length-prefix verdict (P2-17 / R12) ───────
//
// Frames are uint32 length + JSON bytes. The verdicts are NOT symmetric:
//
//   * Zero-length prefix: a protocol violation that consumes no body, so
//     the stream stays byte-aligned. The dispatcher answers `malformed
//     frame` and continues (P2-17/R12).
//   * Over-16 MB prefix: the 4 bytes carry no trustworthy body boundary
//     (the claimed body may never arrive). Draining `len` bytes is
//     unbounded, and treating the next 4 bytes as a new prefix would read
//     payload as length — a permanent desync. The session is unrecoverable
//     and must be torn down, not answered-and-continued (audit 5.5 #3).

#include <cstdint>

namespace rex::wallpaper {

/// Largest accepted frame body (16 MiB — commands are small JSON).
inline constexpr uint32_t kMaxFrameBytes = 16u * 1024u * 1024u;

/// Runtime/host protocol version (P3 / G5). INIT may carry
/// `protocolVersion`; a mismatch fails loudly instead of running on
/// silent defaults. Absent version = pre-handshake host, accepted.
inline constexpr int kProtocolVersion = 1;

inline bool IsSupportedProtocolVersion(int version) {
    return version == kProtocolVersion;
}

/// How to treat a length prefix.
enum class FrameLengthVerdict {
    Ok,         ///< Read `len` body bytes.
    Empty,      ///< Zero-length frame: reject, stream still aligned.
    Oversized,  ///< No trustworthy body boundary: session unrecoverable.
};

inline FrameLengthVerdict ClassifyFrameLength(uint32_t len) {
    if (len == 0) return FrameLengthVerdict::Empty;
    if (len > kMaxFrameBytes) return FrameLengthVerdict::Oversized;
    return FrameLengthVerdict::Ok;
}

}  // namespace rex::wallpaper
