#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rex::theme {

struct ExtractedFile {
    std::string name;              // relative path, forward slashes
    std::vector<uint8_t> data;     // raw bytes (decompressed for ZIP entries)
    uint64_t sizeBytes = 0;
};

// Extracts a .rextheme/.zip archive (Shell COM) or reads a plain directory.
// Extracts only into memory — no files are written to disk (no side effects).
// Returns nullopt on hard failure, populating `errors` with diagnostics.
std::optional<std::vector<ExtractedFile>> ExtractPackage(
    const std::string& packagePath,
    std::vector<std::string>& errors);

// True when the path looks like a zip archive (.rextheme / .zip / .rextheme.*).
bool IsArchivePath(const std::string& packagePath);

} // namespace rex::theme