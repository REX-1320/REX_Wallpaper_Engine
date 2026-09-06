#pragma once
#include "Playlist.hpp"
#include "wallpaper/runtime/RuntimeSettings.hpp"
#include <memory>
#include <string>
#include <vector>
#include <mutex>

namespace rex::wallpaper {

class PlaylistManager {
public:
    explicit PlaylistManager(std::shared_ptr<RuntimeSettings> settings);

    std::vector<Playlist> GetAll() const;
    bool GetById(const std::string& id, Playlist& out) const;

    bool Create(const Playlist& playlist);
    bool Update(const Playlist& playlist);
    bool Delete(const std::string& id);

    bool AddItem(const std::string& playlistId, const PlaylistItem& item);
    bool RemoveItem(const std::string& playlistId, size_t index);
    bool Reorder(const std::string& playlistId, const std::vector<size_t>& order);

    /// Wire-friendly JSON dump of a playlist (used by PLAYLIST_GET).
    std::string Serialize(const Playlist& p) const;

    void LoadAll();
    void SaveAll();

private:
    mutable std::mutex m_mutex;
    std::shared_ptr<RuntimeSettings> m_settings;
    std::vector<Playlist> m_playlists;

    bool Deserialize(const std::string& json, Playlist& out) const;
};

} // namespace rex::wallpaper