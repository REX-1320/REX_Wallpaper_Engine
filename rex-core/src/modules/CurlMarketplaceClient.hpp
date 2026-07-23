#pragma once
#include "IMarketplaceClient.hpp"
#include <string>

namespace rex::modules {

class CurlMarketplaceClient : public IMarketplaceClient {
public:
    explicit CurlMarketplaceClient(const std::string& baseUrl);

    std::vector<PluginManifest> fetchAvailablePlugins() override;
    bool downloadPlugin(const std::string& url, const std::string& destinationPath) override;
    std::string downloadSignature(const std::string& url) override;

private:
    std::string baseUrl_;
    std::string httpGet(const std::string& url);
    bool httpDownload(const std::string& url, const std::string& filePath);
};

} // namespace rex::modules
