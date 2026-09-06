#include "ZipExtractor.hpp"

#include <miniz.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace rex::theme {

namespace {

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::string NormalizeSeparators(std::string name) {
    std::replace(name.begin(), name.end(), '\\', '/');
    return name;
}

bool HasSuffix(const std::string& s, const std::string& suffix) {
    if (suffix.size() > s.size()) return false;
    return s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Read a plain directory into memory (no disk writes).
std::optional<std::vector<ExtractedFile>> ReadDirectory(const std::string& dirPath) {
    std::error_code ec;
    if (!fs::is_directory(dirPath, ec)) {
        return std::nullopt;
    }
    std::vector<ExtractedFile> files;
    fs::recursive_directory_iterator it(dirPath, fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator end;
    for (; it != end && !ec; it.increment(ec)) {
        const auto& entry = *it;
        if (!entry.is_regular_file()) continue;
        std::ifstream in(entry.path(), std::ios::binary);
        if (!in) continue;
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
        std::error_code fec;
        std::string rel = fs::relative(entry.path(), fs::path(dirPath), fec).string();
        files.push_back(ExtractedFile{
            NormalizeSeparators(rel),
            std::move(data),
            static_cast<uint64_t>(data.size())
        });
    }
    return files;
}

// In-memory ZIP extraction via miniz. No files are written to disk.
bool ExtractArchiveMiniz(const std::string& packagePath,
                         std::vector<ExtractedFile>& out,
                         std::vector<std::string>& errors) {
    // Read the archive into memory.
    std::ifstream in(packagePath, std::ios::binary | std::ios::ate);
    if (!in) {
        errors.push_back("cannot open package file: " + packagePath);
        return false;
    }
    const std::streamsize size = in.tellg();
    if (size < 0) {
        errors.push_back("cannot determine package size");
        return false;
    }
    std::vector<uint8_t> archive(static_cast<size_t>(size));
    in.seekg(0, std::ios::beg);
    if (!in.read(reinterpret_cast<char*>(archive.data()), size)) {
        errors.push_back("cannot read package file");
        return false;
    }

    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, archive.data(), archive.size(), 0)) {
        errors.push_back("not a valid ZIP archive");
        return false;
    }

    const mz_uint fileCount = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < fileCount; ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
            errors.push_back("cannot read archive entry metadata");
            continue;
        }
        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            continue;
        }
        std::string name(stat.m_filename);
        name = NormalizeSeparators(name);

        size_t outSize = 0;
        void* p = mz_zip_reader_extract_to_heap(&zip, i, &outSize, 0);
        if (!p) {
            errors.push_back("cannot decompress archive entry: " + name);
            continue;
        }
        std::vector<uint8_t> data(static_cast<const uint8_t*>(p),
                                  static_cast<const uint8_t*>(p) + outSize);
        mz_free(p);
        out.push_back(ExtractedFile{
            std::move(name),
            std::move(data),
            static_cast<uint64_t>(outSize)
        });
    }

    mz_zip_reader_end(&zip);
    return true;
}

} // namespace rex::theme

bool IsArchivePath(const std::string& packagePath) {
    const std::string lower = ToLower(packagePath);
    return HasSuffix(lower, ".rextheme") || HasSuffix(lower, ".zip");
}

std::optional<std::vector<ExtractedFile>> ExtractPackage(
    const std::string& packagePath,
    std::vector<std::string>& errors) {
    if (packagePath.empty()) {
        errors.push_back("empty package path");
        return std::nullopt;
    }

    // Directory package.
    if (!IsArchivePath(packagePath)) {
        auto dirFiles = ReadDirectory(packagePath);
        if (dirFiles) {
            return dirFiles;
        }
        // Fall through to archive attempt below (some paths may lack the
        // extension but still be a zip).
    }

    std::vector<ExtractedFile> files;
    if (ExtractArchiveMiniz(packagePath, files, errors)) {
        return files;
    }
    return std::nullopt;
}

} // namespace rex::theme