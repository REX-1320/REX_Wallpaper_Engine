#include "RankingEngine.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>

namespace rex::search {

static double tokenMatchScore(const std::string& text, const std::vector<std::string>& tokens) {
    if (tokens.empty()) return 0.5;
    double score = 0.0;
    std::string lowerText = text;
    std::transform(lowerText.begin(), lowerText.end(), lowerText.begin(), ::tolower);
    for (auto& token : tokens) {
        auto lowerToken = token;
        std::transform(lowerToken.begin(), lowerToken.end(), lowerToken.begin(), ::tolower);
        if (lowerText.find(lowerToken) != std::string::npos) score += 0.3;
    }
    return std::min(score, 1.0);
}

double RankingEngine::computeSemanticScore(const SearchResult& result, const SearchQuery& query) const {
    (void)query;
    if (semanticScores_.empty()) return 0.0;
    std::string docId = result.providerId + ":" + result.id;
    auto it = semanticScores_.find(docId);
    if (it != semanticScores_.end()) return it->second;
    return 0.0;
}

double RankingEngine::computeScore(const SearchResult& result, const SearchQuery& query) const {
    double score = result.relevanceScore;

    if (result.title == query.rawText) score *= 1.5;

    double tokenScore = tokenMatchScore(result.title + " " + result.description, query.tokens);
    score += tokenScore * 0.2;

    if (!query.context.currentModule.empty() && result.category == query.context.currentModule)
        score *= 1.2;

    auto now = std::chrono::system_clock::now();
    auto age = std::chrono::duration_cast<std::chrono::hours>(now - result.timestamp).count();
    if (age < 24) score += 0.1;
    else if (age < 168) score += 0.05;

    double semantic = computeSemanticScore(result, query);
    score += semantic * 0.3;

    return std::min(score, 1.0);
}

void RankingEngine::rank(std::vector<SearchResult>& results, const SearchQuery& query) const {
    for (auto& r : results) {
        r.relevanceScore = computeScore(r, query);
    }
    std::sort(results.begin(), results.end(), [](const SearchResult& a, const SearchResult& b) {
        return a.relevanceScore > b.relevanceScore;
    });
}

}
