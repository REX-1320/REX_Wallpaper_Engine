#pragma once
#include <vector>
#include <unordered_map>
#include "../models/SearchResult.hpp"
#include "../models/SearchQuery.hpp"

namespace rex::search::semantic { class SemanticIndex; }

namespace rex::search {

class RankingEngine {
public:
    void setSemanticIndex(semantic::SemanticIndex* index) { semanticIndex_ = index; }
    void setSemanticScores(const std::unordered_map<std::string, double>& scores) {
        semanticScores_ = scores;
    }

    void rank(std::vector<SearchResult>& results, const SearchQuery& query) const;

private:
    double computeScore(const SearchResult& result, const SearchQuery& query) const;
    double computeSemanticScore(const SearchResult& result, const SearchQuery& query) const;

    semantic::SemanticIndex* semanticIndex_ = nullptr;
    std::unordered_map<std::string, double> semanticScores_;
};

}
