#pragma once
#include "ISearchProvider.hpp"
#include <functional>

namespace rex::search::semantic {

class ISemanticSearchProvider : public ISearchProvider {
public:
    using IndexCallback = std::function<void(const std::string& providerId,
                                             const std::string& localId,
                                             const std::string& text)>;

    virtual void indexDocuments(IndexCallback indexCallback) = 0;
};

}
