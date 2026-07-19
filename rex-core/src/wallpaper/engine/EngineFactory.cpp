#include <rex/wallpaper/IWallpaperEngine.hpp>
#include "wallpaper/engine/Engine.hpp"
#include <memory>

namespace rex::wallpaper {

std::unique_ptr<IWallpaperEngine> CreateWallpaperEngine() {
    return std::make_unique<Engine>();
}

} // namespace rex::wallpaper
