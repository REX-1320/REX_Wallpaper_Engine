#pragma once
#include <string>
#include <vector>
#include "ModuleMetadata.hpp"

namespace rex::modules {

struct PluginManifest {
    std::string moduleId;
    std::string name;
    std::string version;
    std::string author;
    std::string description;
    std::string category;
    std::string minAppVersion;
    std::vector<std::string> dependencies;
    std::string downloadUrl;
    std::string signatureUrl;
    std::string checksum;
    std::string publicKeyId;
    uint64_t sizeBytes = 0;
    std::string iconUrl;
    std::string releaseDate;
};

} // namespace rex::modules
