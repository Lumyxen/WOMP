#include "womp/playback/PlaybackStats.h"

#include <algorithm>

namespace womp {

void PlaybackStats::start(
    const TrackId& trackId,
    std::optional<PlaylistId> playlistId,
    std::int64_t durationMs,
    std::int64_t wallTimeMs,
    Clock::time_point now)
{
    trackId_ = trackId;
    playlistId_ = playlistId;
    durationMs_ = durationMs;
    sessionListenedMs_ = 0;
    qualified_ = false;
    active_ = true;
    actuallyPlaying_ = false;
    lastTick_ = now;
    pendingLastPlayedAtMs_ = wallTimeMs;
}

void PlaybackStats::accountUntil(Clock::time_point now)
{
    if (!active_) {
        return;
    }
    if (actuallyPlaying_) {
        const auto elapsed = std::max<std::int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTick_).count());
        sessionListenedMs_ += elapsed;
        pendingListenedMs_ += elapsed;
        qualifyIfNeeded(false);
    }
    lastTick_ = now;
}

void PlaybackStats::qualifyIfNeeded(bool naturalEos)
{
    const bool shortNaturalCompletion = naturalEos && durationMs_ > 0 && durationMs_ < 20'000;
    if (!qualified_ && (sessionListenedMs_ >= 20'000 || shortNaturalCompletion)) {
        qualified_ = true;
        ++pendingListenCount_;
    }
}

void PlaybackStats::tick(bool actuallyPlaying, Clock::time_point now)
{
    accountUntil(now);
    actuallyPlaying_ = actuallyPlaying;
}

void PlaybackStats::pause(Clock::time_point now)
{
    accountUntil(now);
    actuallyPlaying_ = false;
}

void PlaybackStats::seek(Clock::time_point now)
{
    accountUntil(now);
}

void PlaybackStats::finish(bool naturalEos, bool userReplaced, Clock::time_point now)
{
    accountUntil(now);
    qualifyIfNeeded(naturalEos);
    if (userReplaced && !qualified_) {
        ++pendingSkipCount_;
    }
    active_ = false;
    actuallyPlaying_ = false;
}

std::int64_t PlaybackStats::pendingListenedMsFor(std::optional<PlaylistId> playlistId) const
{
    return !playlistId || playlistId_ == playlistId ? pendingListenedMs_ : 0;
}

std::vector<StatisticDelta> PlaybackStats::takeDeltas(Clock::time_point now)
{
    accountUntil(now);
    if (trackId_.empty() || (pendingListenedMs_ == 0 && pendingListenCount_ == 0 && pendingSkipCount_ == 0 && !pendingLastPlayedAtMs_)) {
        return {};
    }
    StatisticDelta delta{
        .trackId = trackId_,
        .playlistId = playlistId_,
        .listenedMs = pendingListenedMs_,
        .listenCount = pendingListenCount_,
        .skipCount = pendingSkipCount_,
        .lastPlayedAtMs = pendingLastPlayedAtMs_,
    };
    pendingListenedMs_ = 0;
    pendingListenCount_ = 0;
    pendingSkipCount_ = 0;
    pendingLastPlayedAtMs_.reset();
    return {std::move(delta)};
}

} // namespace womp
