#pragma once
#include <vector>
#include <string>
#include "PluginManifest.hpp"

namespace rex::modules {

class IMarketplaceClient {
public:
    virtual ~IMarketplaceClient() = default;
    virtual std::vector<PluginManifest> fetchAvailablePlugins() = 0;
    virtual bool downloadPlugin(const std::string& url, const std::string& destinationPath) = 0;
    virtual std::string downloadSignature(const std::string& url) = 0;
};

} // namespace rex::modules
