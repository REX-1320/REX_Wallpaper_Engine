#pragma once
#include <string>
#include <any>
#include <chrono>
#include <vector>

namespace rex::search {
struct SearchResult {
    std::string id;
    std::string providerId;
    std::string title;
    std::string description;
    std::string category;
    std::string icon;
    double relevanceScore = 0.0;
    std::any payload;
    std::chrono::system_clock::time_point timestamp;
    std::vector<std::string> tags;
};
}
