#include "PlaylistPlayer.hpp"
#include "PlaylistManager.hpp"
#include <rex/wallpaper/IWallpaperEngine.hpp>
#include <shared/Logger.hpp>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <random>

namespace rex::wallpaper {

namespace {
std::string NowIso8601() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_s(&tm, &t);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return std::string(buf);
}
} // namespace

PlaylistPlayer::PlaylistPlayer(std::shared_ptr<IWallpaperEngine> engine,
                               std::shared_ptr<PlaylistManager> playlistManager)
    : m_engine(engine), m_playlistManager(std::move(playlistManager)) {}

PlaylistPlayer::~PlaylistPlayer() { Stop(); }

bool PlaylistPlayer::Start(const Playlist& playlist, size_t startIndex,
                           bool resume) {
    if (playlist.items.empty()) return false;
    Stop();
    m_playlist = playlist;
    if (resume && m_playlist.lastIndex < m_playlist.items.size()) {
        // Continue from the last played item of a previous session.
        m_currentIndex = m_playlist.lastIndex;
    } else {
        m_currentIndex = startIndex < m_playlist.items.size() ? startIndex : 0;
    }
    m_stopRequested = false;
    m_running = true;
    m_thread = std::thread(&PlaylistPlayer::PlayerLoop, this);
    if (m_eventSink) {
        const auto& item = m_playlist.items[m_currentIndex];
        m_eventSink("started", m_currentIndex, item.path, item.type);
    }
    return true;
}

void PlaylistPlayer::Stop() {
    const bool wasRunning = m_running.load();
    m_stopRequested = true;
    m_running = false;
    if (m_thread.joinable()) m_thread.join();
    if (wasRunning && m_eventSink) {
        m_eventSink("stopped", m_currentIndex, "", "");
    }
}

void PlaylistPlayer::Next() {
    std::lock_guard lock(m_mutex);
    if (!m_running) return;
    size_t idx = NextIndex(m_currentIndex, true);
    LoadAndPlay(idx);
}

void PlaylistPlayer::Previous() {
    std::lock_guard lock(m_mutex);
    if (!m_running) return;
    size_t idx = NextIndex(m_currentIndex, false);
    LoadAndPlay(idx);
}

Playlist PlaylistPlayer::GetCurrentPlaylist() const {
    std::lock_guard lock(m_mutex);
    return m_playlist;
}

void PlaylistPlayer::PlayerLoop() {
    LoadAndPlay(m_currentIndex);
    while (m_running && !m_stopRequested) {
        double duration = GetItemDuration(m_playlist.items[m_currentIndex]);
        // Sleep in small intervals to allow fast stop.
        double elapsed = 0.0;
        while (elapsed < duration && m_running && !m_stopRequested) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            elapsed += 0.1;
        }
        if (m_stopRequested) break;

        // If the item loops, stay on it (but still check stop).
        if (m_playlist.items[m_currentIndex].loop && m_playlist.loopAll) {
            continue;
        }
        std::lock_guard lock(m_mutex);
        size_t idx = NextIndex(m_currentIndex, true);
        LoadAndPlay(idx);
    }
}

void PlaylistPlayer::LoadAndPlay(size_t index) {
    if (index >= m_playlist.items.size()) return;
    m_currentIndex = index;
    const auto& item = m_playlist.items[index];

    SceneInfo scene;
    scene.sourcePath = item.path;
    scene.type = item.type;
    scene.fillMode = item.fillMode;
    scene.volume = item.volume;
    scene.speed = item.speed;
    scene.loop = item.loop;

    auto result = m_engine->LoadScene(scene);
    if (result.IsError()) {
        REX_LOG_WARN("PlaylistPlayer", "Failed to load scene: {}", result.Error().message);
        if (m_eventSink) m_eventSink("item-error", index, item.path, item.type);
        return;
    }
    m_engine->Play();
    // Remember where we are so a restart resumes at this item.
    PersistCurrentPosition();
    if (m_eventSink) m_eventSink("item", index, item.path, item.type);
}

void PlaylistPlayer::PersistCurrentPosition() {
    if (!m_playlistManager) return;
    m_playlist.lastIndex = m_currentIndex;
    m_playlist.updatedAt = NowIso8601();
    m_playlistManager->Update(m_playlist);
}

double PlaylistPlayer::GetItemDuration(const PlaylistItem& item) {
    if (m_playlist.intervalSeconds > 0.0) return m_playlist.intervalSeconds;
    if (item.type == "video") {
        double d = m_engine->GetDuration();
        if (d > 0.0) return d / std::max(0.25f, item.speed);
    }
    // Static/shader: use a default 30s or the interval override.
    return 30.0;
}

size_t PlaylistPlayer::NextIndex(size_t current, bool forward) {
    if (m_playlist.items.empty()) return 0;
    if (m_playlist.shuffle) {
        static std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<size_t> dist(0, m_playlist.items.size() - 1);
        return dist(rng);
    }
    size_t count = m_playlist.items.size();
    size_t next;
    if (forward) {
        next = (current + 1) % count;
        if (next == 0 && !m_playlist.loopAll) {
            // Reached the end and no loop → stop (stay on the last item).
            m_running = false;
            if (m_eventSink) m_eventSink("finished", current, "", "");
            return current;
        }
    } else {
        next = (current == 0) ? count - 1 : current - 1;
    }
    return next;
}

} // namespace rex::wallpaper