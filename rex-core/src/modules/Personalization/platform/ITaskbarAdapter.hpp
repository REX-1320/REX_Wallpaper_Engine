#pragma once
#include "../interfaces/IPersonalizationService.hpp"

namespace rex::personalization::platform {

class ITaskbarAdapter {
public:
    virtual ~ITaskbarAdapter() = default;
    virtual TaskbarSettings read() = 0;
    virtual void apply(const TaskbarSettings& s) = 0;
};

class WinTaskbarAdapter : public ITaskbarAdapter {
public:
    TaskbarSettings read() override;
    void apply(const TaskbarSettings& s) override;
};

} // namespace
