#pragma once

#include "womp/library/Models.h"

#include <cstdint>
#include <optional>
#include <random>
#include <vector>

namespace womp {

class PlaybackQueue {
public:
    explicit PlaybackQueue(std::uint64_t seed = std::random_device{}());

    void start(std::vector<TrackId> source, std::optional<PlaylistId> playlistId, const TrackId& first, bool shuffle);
    void clear();
    void setShuffle(bool enabled);
    std::optional<TrackId> next();
    std::optional<TrackId> previous();

    const std::optional<TrackId>& current() const { return current_; }
    const std::optional<PlaylistId>& sourcePlaylistId() const { return sourcePlaylistId_; }
    bool shuffle() const { return shuffle_; }
    bool empty() const { return !current_.has_value(); }
    const std::vector<TrackId>& history() const { return history_; }
    const std::vector<TrackId>& upcoming() const { return upcoming_; }

private:
    void rebuildUpcomingAfterCurrent();

    std::mt19937_64 random_;
    std::vector<TrackId> source_;
    std::optional<PlaylistId> sourcePlaylistId_;
    std::optional<TrackId> current_;
    std::vector<TrackId> history_;
    std::vector<TrackId> upcoming_;
    bool shuffle_ = false;
};

} // namespace womp
