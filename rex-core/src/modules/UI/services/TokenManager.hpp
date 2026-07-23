#pragma once
#include <string>
#include <unordered_map>
#include <mutex>
#include "../models/Theme.hpp"

namespace rex::ui {

class TokenManager {
public:
    void loadThemeTokens(const Theme& theme,
                         const std::unordered_map<std::string, std::string>& explicitTokens,
                         const TokenManager* parentManager = nullptr);

    std::string getToken(const std::string& tokenPath) const;

    void setTokenOverride(const std::string& tokenPath, const std::string& value);

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::string> tokens_;
};

}
