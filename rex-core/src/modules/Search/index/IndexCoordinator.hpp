#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <unordered_map>
#include "../models/SearchResult.hpp"

namespace rex::search {

class IndexCoordinator {
public:
    void indexDocument(const std::string& providerId, const SearchResult& document);
    void removeDocument(const std::string& providerId, const std::string& documentId);

    std::vector<std::pair<std::string, double>> search(const std::vector<std::string>& tokens,
                                                       const std::string& providerId = "") const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::vector<std::pair<std::string, int>>> invertedIndex_;
    std::unordered_map<std::string, SearchResult> documents_;
};

}
