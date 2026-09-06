#include "PlaylistManager.hpp"
#include "wallpaper/runtime/RuntimeSettings.hpp"
#include "wallpaper/runtime/Json.hpp"
#include <rex/wallpaper/IRendererAdapter.hpp>
#include <shared/Logger.hpp>

#include <algorithm>

namespace rex::wallpaper {

using json = rex::runtime::Json;

PlaylistManager::PlaylistManager(std::shared_ptr<RuntimeSettings> settings)
    : m_settings(settings) {
    LoadAll();
}

std::vector<Playlist> PlaylistManager::GetAll() const {
    std::lock_guard lock(m_mutex);
    return m_playlists;
}

bool PlaylistManager::GetById(const std::string& id, Playlist& out) const {
    std::lock_guard lock(m_mutex);
    for (const auto& p : m_playlists) {
        if (p.id == id) { out = p; return true; }
    }
    return false;
}

bool PlaylistManager::Create(const Playlist& playlist) {
    std::lock_guard lock(m_mutex);
    for (const auto& p : m_playlists) {
        if (p.id == playlist.id) return false;
    }
    m_playlists.push_back(playlist);
    SaveAll();
    return true;
}

bool PlaylistManager::Update(const Playlist& playlist) {
    std::lock_guard lock(m_mutex);
    for (auto& p : m_playlists) {
        if (p.id == playlist.id) {
            p = playlist;
            SaveAll();
            return true;
        }
    }
    return false;
}

bool PlaylistManager::Delete(const std::string& id) {
    std::lock_guard lock(m_mutex);
    auto it = std::find_if(m_playlists.begin(), m_playlists.end(),
        [&](const Playlist& p) { return p.id == id; });
    if (it == m_playlists.end()) return false;
    m_playlists.erase(it);
    if (m_settings) m_settings->RemoveKey("playlist." + id);
    SaveAll();
    return true;
}

bool PlaylistManager::AddItem(const std::string& playlistId, const PlaylistItem& item) {
    std::lock_guard lock(m_mutex);
    for (auto& p : m_playlists) {
        if (p.id == playlistId) {
            p.items.push_back(item);
            SaveAll();
            return true;
        }
    }
    return false;
}

bool PlaylistManager::RemoveItem(const std::string& playlistId, size_t index) {
    std::lock_guard lock(m_mutex);
    for (auto& p : m_playlists) {
        if (p.id == playlistId && index < p.items.size()) {
            p.items.erase(p.items.begin() + index);
            SaveAll();
            return true;
        }
    }
    return false;
}

bool PlaylistManager::Reorder(const std::string& playlistId, const std::vector<size_t>& order) {
    std::lock_guard lock(m_mutex);
    for (auto& p : m_playlists) {
        if (p.id == playlistId) {
            if (order.size() != p.items.size()) return false;
            std::vector<PlaylistItem> reordered;
            for (size_t idx : order) {
                if (idx >= p.items.size()) return false;
                reordered.push_back(p.items[idx]);
            }
            p.items = std::move(reordered);
            SaveAll();
            return true;
        }
    }
    return false;
}

void PlaylistManager::LoadAll() {
    std::lock_guard lock(m_mutex);
    m_playlists.clear();
    if (!m_settings) return;

    const std::string idsStr = m_settings->GetString("playlists.list");
    if (idsStr.empty()) return;

    auto idsJson = json::Parse(idsStr);
    if (!idsJson || !idsJson->IsArray()) return;

    for (const auto& idJson : idsJson->AsArray()) {
        std::string id = idJson.AsString();
        const std::string val = m_settings->GetString("playlist." + id);
        if (!val.empty()) {
            Playlist p;
            if (Deserialize(val, p)) {
                m_playlists.push_back(p);
            }
        }
    }
}

void PlaylistManager::SaveAll() {
    if (!m_settings) return;
    std::vector<std::string> ids;
    for (const auto& p : m_playlists) {
        const std::string key = "playlist." + p.id;
        m_settings->SetString(key, Serialize(p));
        ids.push_back(p.id);
    }
    std::string list = "[";
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i) list += ",";
        list += "\"" + ids[i] + "\"";
    }
    list += "]";
    m_settings->SetString("playlists.list", list);
}

std::string PlaylistManager::Serialize(const Playlist& p) const {
    json::Json::Object o;
    o["id"] = p.id;
    o["name"] = p.name;
    o["shuffle"] = p.shuffle;
    o["loopAll"] = p.loopAll;
    o["intervalSeconds"] = p.intervalSeconds;
    o["lastIndex"] = static_cast<double>(p.lastIndex);
    o["updatedAt"] = p.updatedAt;

    json::Json::Array itemsArr;
    for (const auto& item : p.items) {
        json::Json::Object io;
        io["type"] = item.type;
        io["path"] = item.path;
        io["fillMode"] = std::string(ToString(item.fillMode));
        io["volume"] = item.volume;
        io["speed"] = item.speed;
        io["loop"] = item.loop;
        itemsArr.push_back(json::Json(io));
    }
    o["items"] = json::Json(itemsArr);
    return json::Json(o).Dump();
}

bool PlaylistManager::Deserialize(const std::string& jsonStr, Playlist& out) const {
    auto parsed = json::Parse(jsonStr);
    if (!parsed) return false;
    const auto& o = parsed->AsObject();

    auto it = o.find("id");
    if (it == o.end()) return false;
    out.id = it->second.AsString();

    auto itName = o.find("name");
    out.name = itName != o.end() ? itName->second.AsString() : out.id;

    auto itShuffle = o.find("shuffle");
    out.shuffle = itShuffle != o.end() ? itShuffle->second.AsBool(false) : false;

    auto itLoopAll = o.find("loopAll");
    out.loopAll = itLoopAll != o.end() ? itLoopAll->second.AsBool(true) : true;

    auto itInterval = o.find("intervalSeconds");
    out.intervalSeconds = itInterval != o.end() ? itInterval->second.AsFloat(0.0) : 0.0;

    // Missing fields (playlists written before resume support) default to
    // no resume point — safe for older persisted data.
    auto itLastIdx = o.find("lastIndex");
    out.lastIndex = static_cast<size_t>(
        itLastIdx != o.end() ? itLastIdx->second.AsInt(0) : 0);

    auto itUpdated = o.find("updatedAt");
    out.updatedAt = itUpdated != o.end() ? itUpdated->second.AsString() : "";

    out.items.clear();
    auto itItems = o.find("items");
    if (itItems != o.end() && itItems->second.IsArray()) {
        for (const auto& itemJson : itItems->second.AsArray()) {
            const auto& io = itemJson.AsObject();
            PlaylistItem item;
            auto itType = io.find("type");
            item.type = itType != io.end() ? itType->second.AsString() : "video";
            auto itPath = io.find("path");
            item.path = itPath != io.end() ? itPath->second.AsString() : "";
            auto itFill = io.find("fillMode");
            item.fillMode = itFill != io.end()
                                ? ParseFillMode(itFill->second.AsString("fill"))
                                      .value_or(FillMode::Fill)
                                : FillMode::Fill;
            auto itVol = io.find("volume");
            item.volume = itVol != io.end() ? itVol->second.AsFloat(1.0f) : 1.0f;
            auto itSpeed = io.find("speed");
            item.speed = itSpeed != io.end() ? itSpeed->second.AsFloat(1.0f) : 1.0f;
            auto itLoop = io.find("loop");
            item.loop = itLoop != io.end() ? itLoop->second.AsBool(true) : true;
            out.items.push_back(item);
        }
    }
    return true;
}

} // namespace rex::wallpaper