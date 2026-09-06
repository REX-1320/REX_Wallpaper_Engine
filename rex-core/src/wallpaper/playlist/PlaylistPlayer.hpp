#pragma once
#include "Playlist.hpp"
#include <functional>
#include <memory>
#include <atomic>
#include <mutex>
#include <thread>
#include <optional>

namespace rex::wallpaper {
class IWallpaperEngine;
class PlaylistManager;
}

namespace rex::wallpaper {

/// Playback lifecycle events surfaced to the host: "started", "item"
/// (index/path/type), "stopped", "finished" (end of playlist, no loop).
using PlaylistEventFn = std::function<void(const std::string& event,
                                           size_t index,
                                           const std::string& path,
                                           const std::string& type)>;

class PlaylistPlayer {
public:
    /// playlistManager is optional; without it the current position is not
    /// persisted (no resume across restarts).
    PlaylistPlayer(std::shared_ptr<IWallpaperEngine> engine,
                   std::shared_ptr<PlaylistManager> playlistManager = nullptr);
    ~PlaylistPlayer();

    /// Start playback. With resume=true and a valid playlist.lastIndex the
    /// player continues from that item instead of startIndex (or 0).
    bool Start(const Playlist& playlist, size_t startIndex = 0,
               bool resume = false);
    void Stop();
    void Next();
    void Previous();
    bool IsRunning() const { return m_running; }
    Playlist GetCurrentPlaylist() const;
    size_t GetCurrentIndex() const { return m_currentIndex; }

    void SetEventSink(PlaylistEventFn fn) { m_eventSink = std::move(fn); }

private:
    void PlayerLoop();
    void LoadAndPlay(size_t index);
    double GetItemDuration(const PlaylistItem& item);
    size_t NextIndex(size_t current, bool forward);
    void PersistCurrentPosition();

    std::shared_ptr<IWallpaperEngine> m_engine;
    std::shared_ptr<PlaylistManager> m_playlistManager;
    Playlist m_playlist;
    size_t m_currentIndex = 0;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};
    std::thread m_thread;
    mutable std::mutex m_mutex;
    PlaylistEventFn m_eventSink;
};

} // namespace rex::wallpaper