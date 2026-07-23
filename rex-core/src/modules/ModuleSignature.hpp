#pragma once
#include <string>
#include <cstdint>

namespace rex::modules {

struct ModuleSignature {
    std::string moduleId;
    std::string version;
    std::string minAppVersion;
    std::string checksum;
    uint32_t apiVersion = 1;
};

using GetModuleSignatureFn = const ModuleSignature* (*)();

} // namespace rex::modules
