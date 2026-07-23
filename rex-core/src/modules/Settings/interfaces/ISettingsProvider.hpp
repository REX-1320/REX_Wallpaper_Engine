#pragma once

namespace rex::settings {

class ISettingsService;

class ISettingsProvider {
public:
    virtual ~ISettingsProvider() = default;
    virtual void registerSettings(ISettingsService& service) = 0;
};

}
