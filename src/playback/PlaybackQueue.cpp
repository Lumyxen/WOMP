#include "womp/playback/PlaybackQueue.h"

#include <algorithm>

namespace womp {

PlaybackQueue::PlaybackQueue(std::uint64_t seed)
    : random_(seed)
{
}

void PlaybackQueue::start(
    std::vector<TrackId> source,
    std::optional<PlaylistId> playlistId,
    const TrackId& first,
    bool shuffle)
{
    source_ = std::move(source);
    sourcePlaylistId_ = playlistId;
    if (first.empty() && !source_.empty()) {
        if (shuffle) {
            std::uniform_int_distribution<std::size_t> distribution(0, source_.size() - 1);
            current_ = source_[distribution(random_)];
        } else {
            current_ = source_.front();
        }
    } else {
        current_ = first;
    }
    history_.clear();
    shuffle_ = shuffle;
    rebuildUpcomingAfterCurrent();
}

void PlaybackQueue::clear()
{
    source_.clear();
    sourcePlaylistId_.reset();
    current_.reset();
    history_.clear();
    upcoming_.clear();
}

void PlaybackQueue::rebuildUpcomingAfterCurrent()
{
    upcoming_.clear();
    if (!current_) {
        return;
    }
    const auto current = std::ranges::find(source_, *current_);
    if (shuffle_) {
        for (const TrackId& id : source_) {
            if (id != *current_ && std::ranges::find(history_, id) == history_.end()) {
                upcoming_.push_back(id);
            }
        }
        std::ranges::shuffle(upcoming_, random_);
    } else if (current != source_.end()) {
        upcoming_.assign(std::next(current), source_.end());
    }
}

void PlaybackQueue::setShuffle(bool enabled)
{
    if (shuffle_ == enabled) {
        return;
    }
    shuffle_ = enabled;
    rebuildUpcomingAfterCurrent();
}

std::optional<TrackId> PlaybackQueue::next()
{
    if (!current_ || upcoming_.empty()) {
        if (current_) {
            history_.push_back(*current_);
        }
        current_.reset();
        return std::nullopt;
    }
    history_.push_back(*current_);
    current_ = upcoming_.front();
    upcoming_.erase(upcoming_.begin());
    return current_;
}

std::optional<TrackId> PlaybackQueue::previous()
{
    if (history_.empty()) {
        return std::nullopt;
    }
    if (current_) {
        upcoming_.insert(upcoming_.begin(), *current_);
    }
    current_ = history_.back();
    history_.pop_back();
    return current_;
}

} // namespace womp
