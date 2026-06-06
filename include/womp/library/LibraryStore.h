#pragma once

#include "womp/library/Models.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;

namespace womp {

class LibraryStore {
public:
    explicit LibraryStore(std::filesystem::path dataDirectory);
    ~LibraryStore();

    LibraryStore(const LibraryStore&) = delete;
    LibraryStore& operator=(const LibraryStore&) = delete;

    const std::filesystem::path& dataDirectory() const { return dataDirectory_; }
    std::filesystem::path absoluteTrackPath(const TrackRecord& track) const;
    std::filesystem::path absoluteArtworkPath(const TrackRecord& track) const;

    std::vector<TrackRecord> loadTracks() const;
    std::vector<TrackId> loadTrackIds() const;
    std::vector<PlaylistRecord> loadPlaylists() const;
    std::optional<TrackRecord> findTrack(const TrackId& id) const;
    std::optional<std::string> setting(std::string_view key) const;
    void setSetting(std::string_view key, std::string_view value);

    PlaylistId createPlaylist(std::string_view name);
    bool removePlaylist(PlaylistId id);
    bool setPlaylistPinned(PlaylistId id, bool pinned);
    bool addTrackToPlaylist(PlaylistId playlistId, const TrackId& trackId);
    bool removeTrackFromPlaylist(PlaylistId playlistId, const TrackId& trackId);
    bool removeTrackFromLibrary(const TrackId& trackId);

    void applyImportBatch(
        const std::vector<TrackRecord>& tracks,
        const std::vector<std::pair<PlaylistId, TrackId>>& memberships);
    void applyStatisticDeltas(const std::vector<StatisticDelta>& deltas);
    PlaylistSummary summary(std::optional<PlaylistId> playlistId) const;

    static std::filesystem::path defaultDataDirectory();

private:
    void initialize();
    void execute(std::string_view sql) const;
    void migrateLegacyState();
    void removeLegacyDefaultPlaylists();

    std::filesystem::path dataDirectory_;
    sqlite3* database_ = nullptr;
    mutable std::recursive_mutex databaseMutex_;
};

} // namespace womp
