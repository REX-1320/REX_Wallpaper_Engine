#pragma once
#include <atomic>
#include <memory>

namespace rex::search {

class ICancellationToken {
public:
    virtual bool isCancelled() const = 0;
    virtual ~ICancellationToken() = default;
};

class SimpleCancellationToken : public ICancellationToken {
public:
    bool isCancelled() const override { return cancelled_.load(); }
    void cancel() { cancelled_.store(true); }
private:
    std::atomic<bool> cancelled_{false};
};

}
