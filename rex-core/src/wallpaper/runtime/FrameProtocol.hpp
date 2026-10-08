#pragma once

// ─── Stdin/stdout frame protocol: length-prefix verdict (P2-17 / R12) ───────
//
// Frames are uint32 length + JSON bytes. A zero-length prefix is a protocol
// violation, not EOF; an over-16 MB prefix is corrupt input, not a session
// killer. Both answer `malformed frame` and continue — the dispatch loop
// only tears down on a genuinely closed/truncated stream.

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
enum class FrameLengthVerdict { Ok, Malformed };

inline FrameLengthVerdict ClassifyFrameLength(uint32_t len) {
    return (len == 0 || len > kMaxFrameBytes) ? FrameLengthVerdict::Malformed
                                              : FrameLengthVerdict::Ok;
}

}  // namespace rex::wallpaper
