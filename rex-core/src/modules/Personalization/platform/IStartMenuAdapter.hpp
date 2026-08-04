#pragma once
#include "../interfaces/IPersonalizationService.hpp"

namespace rex::personalization::platform {

class IStartMenuAdapter {
public:
    virtual ~IStartMenuAdapter() = default;
    virtual StartMenuSettings read() = 0;
    virtual void apply(const StartMenuSettings& s) = 0;
};

class WinStartMenuAdapter : public IStartMenuAdapter {
public:
    StartMenuSettings read() override;
    void apply(const StartMenuSettings& s) override;
};

} // namespace
