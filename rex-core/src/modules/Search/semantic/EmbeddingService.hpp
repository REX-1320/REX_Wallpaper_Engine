#pragma once
#include <vector>
#include <memory>
#include <string>

namespace rex::search::semantic {

using Embedding = std::vector<float>;

class IEmbeddingService {
public:
    virtual ~IEmbeddingService() = default;
    virtual Embedding embed(const std::string& text) = 0;
    virtual int dimension() const = 0;
};

}
