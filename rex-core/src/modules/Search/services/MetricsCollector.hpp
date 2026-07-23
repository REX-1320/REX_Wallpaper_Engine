#pragma once
#include <string>
#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>

namespace rex::search {

struct SearchMetrics {
    int totalQueries = 0;
    int totalTimeouts = 0;
    std::chrono::steady_clock::duration totalLatency{0};
    std::unordered_map<std::string, int> providerQueries;
};

class MetricsCollector {
public:
    void recordQuery(const std::string& providerId, std::chrono::milliseconds latency);
    void recordTimeout(const std::string& providerId);
    SearchMetrics getMetrics() const;
    void reset();

private:
    mutable std::mutex mutex_;
    SearchMetrics metrics_;
};

}
