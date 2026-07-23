#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <memory>
#include <optional>
#include "EmbeddingService.hpp"
#include "../models/SearchResult.hpp"

namespace rex::search::semantic {

struct ScoredDoc {
    std::string documentId;
    double score;
};

class SemanticIndex {
public:
    explicit SemanticIndex(std::shared_ptr<IEmbeddingService> embeddingService);

    void indexDocument(const std::string& providerId,
                       const std::string& localId,
                       const std::string& text);

    void indexDocument(const std::string& providerId,
                       const std::string& localId,
                       const std::string& text,
                       const SearchResult& fullResult);

    void removeDocument(const std::string& providerId, const std::string& localId);

    std::vector<ScoredDoc> search(const std::string& query, int topK = 10) const;

    std::vector<ScoredDoc> searchWithResults(const std::string& query, int topK,
                                             std::vector<SearchResult>& outResults) const;

    std::optional<SearchResult> getDocument(const std::string& docId) const;

private:
    std::shared_ptr<IEmbeddingService> embedder_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Embedding> store_;
    std::unordered_map<std::string, SearchResult> resultsStore_;

    static double cosineSimilarity(const Embedding& a, const Embedding& b);
};

}
