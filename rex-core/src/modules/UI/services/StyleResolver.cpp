#include "StyleResolver.hpp"
#include "TokenManager.hpp"

namespace rex::ui {

StyleResolver::StyleResolver(const TokenManager& tokenManager)
    : tokenManager_(tokenManager) {}

std::unordered_map<std::string, std::string> StyleResolver::resolve(
    const ComponentStyleDefinition& def,
    const std::string& variant,
    const std::string& state) const {

    std::unordered_map<std::string, std::string> result = def.defaultProperties;

    if (!variant.empty()) {
        for (auto& v : def.variants) {
            if (v.name == variant) {
                for (auto& [key, tokenPath] : v.properties) {
                    result[key] = tokenPath;
                }
                break;
            }
        }
    }

    for (auto& [key, tokenPath] : result) {
        if (!state.empty()) {
            std::string stateTokenPath = tokenPath + "." + state;
            std::string stateValue = tokenManager_.getToken(stateTokenPath);
            if (!stateValue.empty()) {
                result[key] = stateValue;
                continue;
            }
        }
        result[key] = tokenManager_.getToken(tokenPath);
    }

    return result;
}

}
