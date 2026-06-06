#include "womp/playback/AudioPlayer.h"

#include <gst/gst.h>

#include <algorithm>
#include <mutex>
#include <stdexcept>

namespace womp {

namespace {

void initializeGstreamer()
{
    static std::once_flag flag;
    std::call_once(flag, [] {
        GError* error = nullptr;
        if (!gst_init_check(nullptr, nullptr, &error)) {
            const std::string message = error == nullptr ? "unknown error" : error->message;
            g_clear_error(&error);
            throw std::runtime_error("failed to initialize GStreamer: " + message);
        }
    });
}

} // namespace

AudioPlayer::AudioPlayer(bool fakeSink)
{
    initializeGstreamer();
    player_ = gst_element_factory_make("playbin3", "player");
    if (player_ == nullptr) {
        player_ = gst_element_factory_make("playbin", "player");
    }
    if (player_ == nullptr) {
        throw std::runtime_error("GStreamer playbin is unavailable");
    }
    if (fakeSink) {
        GstElement* sink = gst_element_factory_make("fakesink", "audio-fakesink");
        if (sink == nullptr) {
            throw std::runtime_error("GStreamer fakesink is unavailable");
        }
        g_object_set(player_, "audio-sink", sink, nullptr);
    }
}

AudioPlayer::~AudioPlayer()
{
    if (player_ != nullptr) {
        gst_element_set_state(player_, GST_STATE_NULL);
        gst_object_unref(player_);
    }
}

bool AudioPlayer::play(const std::filesystem::path& path)
{
    gchar* uri = gst_filename_to_uri(path.c_str(), nullptr);
    if (uri == nullptr) {
        return false;
    }

    if (!resetForNextTrack()) {
        g_free(uri);
        return false;
    }

    g_object_set(player_, "uri", uri, nullptr);
    g_free(uri);
    applyAudioSettings();
    const bool started = gst_element_set_state(player_, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;
    applyAudioSettings();
    return started;
}

void AudioPlayer::resume()
{
    gst_element_set_state(player_, GST_STATE_PLAYING);
    applyAudioSettings();
}

void AudioPlayer::pause()
{
    gst_element_set_state(player_, GST_STATE_PAUSED);
}

bool AudioPlayer::seek(std::int64_t positionMs)
{
    return gst_element_seek_simple(
        player_,
        GST_FORMAT_TIME,
        static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
        std::max<std::int64_t>(0, positionMs) * GST_MSECOND);
}

void AudioPlayer::setVolume(double volume)
{
    cubicVolume_ = std::clamp(volume, 0.0, 1.0);
    applyAudioSettings();
}

void AudioPlayer::setMuted(bool muted)
{
    muted_ = muted;
    applyAudioSettings();
}

std::int64_t AudioPlayer::positionMs() const
{
    gint64 position = 0;
    return gst_element_query_position(player_, GST_FORMAT_TIME, &position) ? position / GST_MSECOND : 0;
}

std::int64_t AudioPlayer::durationMs() const
{
    gint64 duration = 0;
    return gst_element_query_duration(player_, GST_FORMAT_TIME, &duration) ? duration / GST_MSECOND : 0;
}

bool AudioPlayer::actuallyPlaying() const
{
    GstState state = GST_STATE_NULL;
    gst_element_get_state(player_, &state, nullptr, 0);
    return state == GST_STATE_PLAYING;
}

std::vector<AudioPlayer::Event> AudioPlayer::pollEvents()
{
    std::vector<Event> events;
    GstBus* bus = gst_element_get_bus(player_);
    while (GstMessage* message = gst_bus_pop_filtered(
               bus,
               static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR | GST_MESSAGE_STATE_CHANGED))) {
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
            events.push_back({.type = EventType::Eos});
        } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            GError* error = nullptr;
            gchar* debug = nullptr;
            gst_message_parse_error(message, &error, &debug);
            events.push_back({.type = EventType::Error, .message = error == nullptr ? "playback error" : error->message});
            g_clear_error(&error);
            g_free(debug);
        } else if (GST_MESSAGE_SRC(message) == GST_OBJECT(player_)) {
            events.push_back({.type = EventType::StateChanged});
        }
        gst_message_unref(message);
    }
    gst_object_unref(bus);
    return events;
}

bool AudioPlayer::resetForNextTrack()
{
    GstStateChangeReturn result = gst_element_set_state(player_, GST_STATE_NULL);
    if (result == GST_STATE_CHANGE_FAILURE) {
        return false;
    }
    if (result == GST_STATE_CHANGE_ASYNC) {
        result = gst_element_get_state(player_, nullptr, nullptr, 5 * GST_SECOND);
        if (result == GST_STATE_CHANGE_FAILURE || result == GST_STATE_CHANGE_ASYNC) {
            return false;
        }
    }

    GstBus* bus = gst_element_get_bus(player_);
    while (GstMessage* message = gst_bus_pop(bus)) {
        gst_message_unref(message);
    }
    gst_object_unref(bus);
    return true;
}

void AudioPlayer::applyAudioSettings()
{
    const double linearVolume = cubicVolume_ * cubicVolume_ * cubicVolume_;
    g_object_set(
        player_,
        "volume", linearVolume,
        "mute", static_cast<gboolean>(muted_),
        nullptr);
}

} // namespace womp
