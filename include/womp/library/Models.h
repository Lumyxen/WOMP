#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace womp {

using TrackId = std::string;
using PlaylistId = std::int64_t;

struct TrackStats {
    std::int64_t listenedMs = 0;
    std::int64_t listenCount = 0;
    std::int64_t skipCount = 0;
    std::optional<std::int64_t> lastPlayedAtMs;
};

struct TrackRecord {
    TrackId id;
    std::filesystem::path relativePath;
    std::string title;
    std::string album;
    std::vector<std::string> artists;
    std::int64_t durationMs = 0;
    std::string formatLabel;
    std::filesystem::path artworkRelativePath;
    std::int64_t addedAtMs = 0;
    bool available = true;
    TrackStats stats;
};

struct PlaylistRecord {
    PlaylistId id = 0;
    std::string name;
    std::int64_t createdAtMs = 0;
    std::int64_t position = 0;
    bool pinned = false;
    std::vector<TrackId> trackIds;
};

struct StatisticDelta {
    TrackId trackId;
    std::optional<PlaylistId> playlistId;
    std::int64_t listenedMs = 0;
    std::int64_t listenCount = 0;
    std::int64_t skipCount = 0;
    std::optional<std::int64_t> lastPlayedAtMs;
};

struct PlaylistSummary {
    std::int64_t totalDurationMs = 0;
    std::int64_t listenedMs = 0;
    std::optional<std::int64_t> lastPlayedAtMs;
};

} // namespace womp
