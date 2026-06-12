#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct _GstElement;

namespace womp {

class AudioPlayer {
public:
    struct SpectrumFrame {
        std::vector<std::vector<float>> channelMagnitudesDb;
    };

    enum class EventType { Eos, Error, StateChanged, Seeked, Spectrum };
    struct Event {
        EventType type = EventType::StateChanged;
        std::string message;
        SpectrumFrame spectrum;
    };

    explicit AudioPlayer(bool fakeSink = false);
    ~AudioPlayer();

    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    bool play(const std::filesystem::path& path);
    void resume();
    void pause();
    bool seek(std::int64_t positionMs);
    // Volume uses GStreamer's cubic UI scale, where 0.0 is silent and 1.0 is full volume.
    void setVolume(double volume);
    void setMuted(bool muted);
    std::int64_t positionMs() const;
    std::int64_t durationMs() const;
    bool actuallyPlaying() const;
    std::vector<Event> pollEvents();

private:
    bool resetForNextTrack();
    void applyAudioSettings();

    _GstElement* player_ = nullptr;
    double cubicVolume_ = 1.0;
    bool muted_ = false;
};

} // namespace womp
