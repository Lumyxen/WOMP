#pragma once

#include "womp/library/LibraryStore.h"

#include <filesystem>
#include <optional>
#include <vector>

struct _GstDiscoverer;

namespace womp {

struct ImportResult {
    std::size_t imported = 0;
    std::size_t duplicates = 0;
    std::size_t unsupported = 0;
    std::size_t failed = 0;
    std::vector<TrackId> trackIds;
};

class ImportService {
public:
    explicit ImportService(LibraryStore& store);

    ImportResult importFiles(
        std::vector<std::filesystem::path> paths,
        const std::vector<PlaylistId>& playlistIds = {});
    ImportResult importM3u(const std::filesystem::path& path, PlaylistId playlistId);

private:
    struct Metadata {
        bool playable = false;
        std::string title;
        std::string album;
        std::vector<std::string> artists;
        std::int64_t durationMs = 0;
        std::string formatLabel;
        std::vector<std::uint8_t> artwork;
    };

    Metadata discover(const std::filesystem::path& path, _GstDiscoverer* discoverer) const;
    std::optional<TrackId> copyAndHash(const std::filesystem::path& source) const;
    std::filesystem::path cacheArtwork(
        const TrackId& id,
        const std::filesystem::path& source,
        const std::vector<std::uint8_t>& embedded) const;
    ImportResult importPaths(
        std::vector<std::filesystem::path> paths,
        const std::vector<PlaylistId>& playlistIds,
        bool sortPaths);

    LibraryStore& store_;
};

} // namespace womp
