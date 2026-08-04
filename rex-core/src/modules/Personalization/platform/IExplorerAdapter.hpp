#pragma once
#include "../interfaces/IPersonalizationService.hpp"

namespace rex::personalization::platform {

class IExplorerAdapter {
public:
    virtual ~IExplorerAdapter() = default;
    virtual ExplorerSettings read() = 0;
    virtual void apply(const ExplorerSettings& s) = 0;
};

class WinExplorerAdapter : public IExplorerAdapter {
public:
    ExplorerSettings read() override;
    void apply(const ExplorerSettings& s) override;
};

} // namespace
