#include "UpdateManager.hpp"
#include "ModuleOrchestrator.hpp"
#include "IMarketplaceClient.hpp"
#include "SignatureVerifier.hpp"
#include "ModuleLoader.hpp"
#include "ModuleRegistry.hpp"
#include <cctype>
#include <iostream>
#include <fstream>
#include <filesystem>

namespace rex::modules {

UpdateManager::UpdateManager(ModuleOrchestrator& manager,
                             std::unique_ptr<IMarketplaceClient> client,
                             std::shared_ptr<SignatureVerifier> verifier)
    : manager_(manager), marketplaceClient_(std::move(client)), verifier_(std::move(verifier)) {}

std::vector<PluginManifest> UpdateManager::checkForUpdates() {
    std::vector<PluginManifest> updates;
    auto available = marketplaceClient_->fetchAvailablePlugins();

    for (auto mod : manager_.getRegistry().getAllModules()) {
        const auto& localMeta = mod->getMetadata();
        for (const auto& remote : available) {
            if (remote.moduleId == localMeta.id) {
                if (remote.version > localMeta.version) {
                    updates.push_back(remote);
                }
                break;
            }
        }
    }
    return updates;
}

namespace {
bool IsSafePathSegment(const std::string& s) {
    if (s.empty() || s.size() > 128) return false;
    for (char c : s) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-'))
            return false;
    }
    if (s == "." || s == "..") return false;
    return s.find("..") == std::string::npos;
}
} // namespace

std::string UpdateManager::getDownloadPath(const std::string& moduleId, const std::string& version) const {
    // Reject traversal: moduleId/version come from marketplace metadata.
    if (!IsSafePathSegment(moduleId) || !IsSafePathSegment(version)) {
        return {};
    }
    std::error_code ec;
    std::filesystem::create_directories(downloadDir_, ec);
    auto full = std::filesystem::weakly_canonical(
        std::filesystem::path(downloadDir_) / (moduleId + "-" + version + ".rexplg"), ec);
    auto base = std::filesystem::weakly_canonical(std::filesystem::path(downloadDir_), ec);
    if (ec || full.string().rfind(base.string(), 0) != 0) {
        return {};
    }
    return full.string();
}

bool UpdateManager::installOrUpdate(const std::string& moduleId) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto allRemote = marketplaceClient_->fetchAvailablePlugins();
    const PluginManifest* remote = nullptr;
    for (auto& m : allRemote) {
        if (m.moduleId == moduleId) {
            remote = &m;
            break;
        }
    }
    if (!remote) {
        std::cerr << "Module not found in marketplace: " << moduleId << "\n";
        return false;
    }

    std::string localPath = getDownloadPath(moduleId, remote->version);
    if (localPath.empty()) {
        std::cerr << "Rejected unsafe module id/version: " << moduleId << "\n";
        return false;
    }

    if (!marketplaceClient_->downloadPlugin(remote->downloadUrl, localPath)) {
        std::cerr << "Failed to download plugin binary\n";
        return false;
    }

    std::string sigData;
    try {
        sigData = marketplaceClient_->downloadSignature(remote->signatureUrl);
    } catch (...) {
        std::cerr << "Failed to download signature\n";
        return false;
    }

    if (!verifier_->verifyFile(localPath, sigData, remote->publicKeyId)) {
        std::cerr << "Signature verification failed for " << moduleId << "\n";
        std::filesystem::remove(localPath);
        return false;
    }

    auto existingMod = manager_.getRegistry().getModule(moduleId);
    bool isUpdate = (existingMod != nullptr);
    if (isUpdate) {
        manager_.stopModule(moduleId);
        // Drop our reference BEFORE unloadDynamic unmaps the DLL: the
        // shared_ptr keeps the object alive but not its code. Re-fetch
        // after load; see ModuleLoader::unloadDynamic contract.
        existingMod.reset();
        if (manager_.getLoader().isDynamic(moduleId)) {
            manager_.getLoader().unloadDynamic(moduleId);
            manager_.getRegistry().unregisterModule(moduleId);
        } else {
            std::cerr << "Cannot update built-in module via marketplace\n";
            return false;
        }
    }

    bool loaded = manager_.loadDynamicModule(localPath);
    if (!loaded) {
        std::cerr << "Failed to load new plugin after download\n";
        return false;
    }

    if (isUpdate) {
        manager_.startModule(moduleId);
        std::cout << "Updated and started " << moduleId << "\n";
    } else {
        std::cout << "Installed new plugin: " << moduleId << "\n";
    }
    return true;
}

bool UpdateManager::installPlugin(const std::string& moduleId) {
    return installOrUpdate(moduleId);
}

} // namespace rex::modules
