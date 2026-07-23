#include "MetricsCollector.hpp"

namespace rex::search {

void MetricsCollector::recordQuery(const std::string& providerId, std::chrono::milliseconds latency) {
    std::lock_guard lock(mutex_);
    metrics_.totalQueries++;
    metrics_.totalLatency += latency;
    metrics_.providerQueries[providerId]++;
}

void MetricsCollector::recordTimeout(const std::string& providerId) {
    std::lock_guard lock(mutex_);
    metrics_.totalTimeouts++;
    metrics_.providerQueries[providerId]++;
}

SearchMetrics MetricsCollector::getMetrics() const {
    std::lock_guard lock(mutex_);
    return metrics_;
}

void MetricsCollector::reset() {
    std::lock_guard lock(mutex_);
    metrics_ = SearchMetrics{};
}

}
