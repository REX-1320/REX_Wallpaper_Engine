#pragma once

#include <windows.h>
#include <cstdint>

namespace rex::platform::windows {

/// Stable monitor identity: FNV-1a hash of the monitor's EDID descriptor
/// block (the physical panel, not the connection). Returns 0 when the
/// physical monitor cannot be queried (remote sessions, some drivers).
uint64_t GetMonitorEdidHash(HMONITOR monitor);

} // namespace rex::platform::windows