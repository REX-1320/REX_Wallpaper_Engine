#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace rex::modules {

struct ModuleMetadata {
    std::string id;
    std::string name;
    std::string version;
    std::string author;
    std::string description;
    std::string category;
    std::string icon;

    std::vector<std::string> dependencies;
    std::string minAppVersion;
    bool experimental = false;
    bool builtIn = true;
    bool autoStart = false;
    bool enabledByDefault = true;

    std::vector<std::string> permissions;
};

} // namespace rex::modules
