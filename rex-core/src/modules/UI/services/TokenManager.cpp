#include "TokenManager.hpp"

namespace rex::ui {

void TokenManager::loadThemeTokens(const Theme& theme,
                                   const std::unordered_map<std::string, std::string>& explicitTokens,
                                   const TokenManager* parentManager) {
    std::lock_guard lock(mutex_);
    if (parentManager) {
        tokens_ = parentManager->tokens_;
    } else {
        tokens_.clear();
    }
    for (auto& [path, value] : explicitTokens) {
        tokens_[path] = value;
    }
}

std::string TokenManager::getToken(const std::string& tokenPath) const {
    std::lock_guard lock(mutex_);
    auto it = tokens_.find(tokenPath);
    if (it != tokens_.end()) return it->second;
    return "";
}

void TokenManager::setTokenOverride(const std::string& tokenPath, const std::string& value) {
    std::lock_guard lock(mutex_);
    tokens_[tokenPath] = value;
}

}
