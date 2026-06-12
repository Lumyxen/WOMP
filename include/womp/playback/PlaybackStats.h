#pragma once

#include "womp/library/Models.h"

#include <chrono>
#include <optional>
#include <vector>

namespace womp {

class PlaybackStats {
public:
    using Clock = std::chrono::steady_clock;

    void start(const TrackId& trackId, std::optional<PlaylistId> playlistId, std::int64_t durationMs, std::int64_t wallTimeMs, Clock::time_point now);
    void tick(bool actuallyPlaying, Clock::time_point now);
    void pause(Clock::time_point now);
    void seek(Clock::time_point now);
    void finish(bool naturalEos, bool userReplaced, Clock::time_point now);
    std::vector<StatisticDelta> takeDeltas(Clock::time_point now);
    bool active() const { return active_; }
    std::int64_t pendingListenedMsFor(std::optional<PlaylistId> playlistId) const;

private:
    void accountUntil(Clock::time_point now);
    void qualifyIfNeeded(bool naturalEos);

    TrackId trackId_;
    std::optional<PlaylistId> playlistId_;
    std::int64_t durationMs_ = 0;
    std::int64_t sessionListenedMs_ = 0;
    std::int64_t pendingListenedMs_ = 0;
    std::int64_t pendingListenCount_ = 0;
    std::int64_t pendingSkipCount_ = 0;
    std::optional<std::int64_t> pendingLastPlayedAtMs_;
    Clock::time_point lastTick_{};
    bool active_ = false;
    bool actuallyPlaying_ = false;
    bool qualified_ = false;
};

} // namespace womp
