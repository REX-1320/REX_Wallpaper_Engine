#include "SemanticIndex.hpp"
#include <algorithm>
#include <cmath>

namespace rex::search::semantic {

SemanticIndex::SemanticIndex(std::shared_ptr<IEmbeddingService> embeddingService)
    : embedder_(std::move(embeddingService)) {}

void SemanticIndex::indexDocument(const std::string& providerId,
                                  const std::string& localId,
                                  const std::string& text) {
    std::string fullId = providerId + ":" + localId;
    auto emb = embedder_->embed(text);
    std::lock_guard lock(mutex_);
    store_[fullId] = std::move(emb);
}

void SemanticIndex::indexDocument(const std::string& providerId,
                                  const std::string& localId,
                                  const std::string& text,
                                  const SearchResult& fullResult) {
    std::string fullId = providerId + ":" + localId;
    auto emb = embedder_->embed(text);
    std::lock_guard lock(mutex_);
    store_[fullId] = std::move(emb);
    resultsStore_[fullId] = fullResult;
}

void SemanticIndex::removeDocument(const std::string& providerId, const std::string& localId) {
    std::lock_guard lock(mutex_);
    store_.erase(providerId + ":" + localId);
    resultsStore_.erase(providerId + ":" + localId);
}

double SemanticIndex::cosineSimilarity(const Embedding& a, const Embedding& b) {
    if (a.size() != b.size()) return 0.0;
    double dot = 0.0, normA = 0.0, normB = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += a[i] * b[i];
        normA += a[i] * a[i];
        normB += b[i] * b[i];
    }
    if (normA == 0.0 || normB == 0.0) return 0.0;
    return dot / (std::sqrt(normA) * std::sqrt(normB));
}

std::vector<ScoredDoc> SemanticIndex::search(const std::string& query, int topK) const {
    auto queryEmb = embedder_->embed(query);
    std::lock_guard lock(mutex_);
    std::vector<ScoredDoc> scored;
    for (const auto& [docId, docEmb] : store_) {
        double sim = cosineSimilarity(queryEmb, docEmb);
        scored.push_back({docId, sim});
    }
    std::sort(scored.begin(), scored.end(),
              [](const ScoredDoc& a, const ScoredDoc& b) { return a.score > b.score; });
    if (scored.size() > static_cast<size_t>(topK)) scored.resize(topK);
    return scored;
}

std::vector<ScoredDoc> SemanticIndex::searchWithResults(const std::string& query, int topK,
                                                         std::vector<SearchResult>& outResults) const {
    auto hits = search(query, topK);
    std::lock_guard lock(mutex_);
    for (auto& hit : hits) {
        auto it = resultsStore_.find(hit.documentId);
        if (it != resultsStore_.end()) {
            outResults.push_back(it->second);
        } else {
            SearchResult r;
            r.id = hit.documentId;
            r.title = hit.documentId;
            r.relevanceScore = hit.score;
            outResults.push_back(r);
        }
    }
    return hits;
}

std::optional<SearchResult> SemanticIndex::getDocument(const std::string& docId) const {
    std::lock_guard lock(mutex_);
    auto it = resultsStore_.find(docId);
    if (it != resultsStore_.end()) return it->second;
    return std::nullopt;
}

}
