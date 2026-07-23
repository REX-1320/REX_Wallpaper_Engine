#pragma once

#include "IDesktopHost.hpp"
#include <memory>
#include <string>

namespace rex::platform::windows {

class DesktopHostFactory {
public:
    static std::unique_ptr<IDesktopHost> CreateBestAvailable();
    static std::unique_ptr<IDesktopHost> Create(const std::string& type);

private:
    DesktopHostFactory() = delete;
};

} // namespace rex::platform::windows
