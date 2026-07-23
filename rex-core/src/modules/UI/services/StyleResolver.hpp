#pragma once
#include <string>
#include <unordered_map>
#include "../models/ComponentStyle.hpp"

namespace rex::ui {

class TokenManager;

class StyleResolver {
public:
    explicit StyleResolver(const TokenManager& tokenManager);

    std::unordered_map<std::string, std::string> resolve(
        const ComponentStyleDefinition& def,
        const std::string& variant,
        const std::string& state) const;

private:
    const TokenManager& tokenManager_;
};

}
