#pragma once
#include <string>
#include <vector>
#include <rex/wallpaper/IWallpaperEngine.hpp>

namespace rex::wallpaper {

struct PlaylistItem {
    std::string type;       // "static", "video", "shader", ...
    std::string path;
    FillMode fillMode = FillMode::Fill;
    float volume = 1.0f;
    float speed = 1.0f;
    bool loop = true;       // per-item loop (if true, never advances)
};

struct Playlist {
    std::string id;
    std::string name;
    std::vector<PlaylistItem> items;
    bool shuffle = false;
    bool loopAll = true;          // when true, after last item go to first
    double intervalSeconds = 0.0; // 0 = use video duration; >0 overrides
    size_t lastIndex = 0;         // resume point: last played item
    std::string updatedAt;        // ISO-8601 UTC of the last position update
};

} // namespace rex::wallpaper