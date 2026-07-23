#include "IndexCoordinator.hpp"
#include <algorithm>
#include <cctype>
#include <sstream>

namespace rex::search {

static std::vector<std::string> tokenize(const std::string& text) {
    std::vector<std::string> tokens;
    std::istringstream iss(text);
    std::string word;
    while (iss >> word) {
        word.erase(std::remove_if(word.begin(), word.end(), ::ispunct), word.end());
        std::transform(word.begin(), word.end(), word.begin(), ::tolower);
        if (!word.empty()) tokens.push_back(word);
    }
    return tokens;
}

void IndexCoordinator::indexDocument(const std::string& providerId, const SearchResult& document) {
    std::lock_guard lock(mutex_);
    std::string docId = providerId + ":" + document.id;
    documents_[docId] = document;
    auto tokens = tokenize(document.title + " " + document.description);
    for (auto& token : tokens) {
        auto& list = invertedIndex_[token];
        auto it = std::find_if(list.begin(), list.end(),
                               [&](auto& p) { return p.first == docId; });
        if (it != list.end()) {
            it->second++;
        } else {
            list.push_back({docId, 1});
        }
    }
}

void IndexCoordinator::removeDocument(const std::string& providerId, const std::string& documentId) {
    std::lock_guard lock(mutex_);
    std::string docId = providerId + ":" + documentId;
    documents_.erase(docId);
    for (auto it = invertedIndex_.begin(); it != invertedIndex_.end(); ) {
        auto& list = it->second;
        list.erase(std::remove_if(list.begin(), list.end(),
            [&](auto& p) { return p.first == docId; }), list.end());
        if (list.empty()) {
            it = invertedIndex_.erase(it);
        } else {
            ++it;
        }
    }
}

std::vector<std::pair<std::string, double>> IndexCoordinator::search(
    const std::vector<std::string>& tokens, const std::string& providerId) const {
    std::lock_guard lock(mutex_);
    std::unordered_map<std::string, double> scores;
    for (auto& token : tokens) {
        std::string lower = token;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        auto it = invertedIndex_.find(lower);
        if (it != invertedIndex_.end()) {
            for (auto& [docId, freq] : it->second) {
                scores[docId] += freq;
            }
        }
    }
    std::vector<std::pair<std::string, double>> ranked;
    for (auto& [docId, score] : scores) {
        if (!providerId.empty() && docId.substr(0, providerId.size()) != providerId) continue;
        ranked.push_back({docId, score});
    }
    std::sort(ranked.begin(), ranked.end(),
              [](auto& a, auto& b) { return a.second > b.second; });
    return ranked;
}

}
