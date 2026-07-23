#pragma once

#include <memory>
#include <string>
#include <vector>
#include <mutex>
#include "PluginManifest.hpp"
#include "IMarketplaceClient.hpp"

namespace rex::modules {

class ModuleOrchestrator;
class IMarketplaceClient;
class SignatureVerifier;
class ModuleLoader;

class UpdateManager {
public:
    UpdateManager(ModuleOrchestrator& manager, std::unique_ptr<IMarketplaceClient> client,
                  std::shared_ptr<SignatureVerifier> verifier);

    std::vector<PluginManifest> checkForUpdates();
    bool installOrUpdate(const std::string& moduleId);
    bool installPlugin(const std::string& moduleId);

private:
    std::string getDownloadPath(const std::string& moduleId, const std::string& version) const;

    ModuleOrchestrator& manager_;
    std::unique_ptr<IMarketplaceClient> marketplaceClient_;
    std::shared_ptr<SignatureVerifier> verifier_;
    std::mutex mutex_;
    std::string downloadDir_ = "plugins/cache";
};

} // namespace rex::modules
