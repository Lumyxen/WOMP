#include "womp/playback/AudioPlayer.h"

#include <gst/gst.h>

#include <algorithm>
#include <mutex>
#include <optional>
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

std::optional<float> spectrumMagnitude(const GValue* value)
{
    if (G_VALUE_HOLDS_FLOAT(value)) {
        return g_value_get_float(value);
    }
    if (G_VALUE_HOLDS_DOUBLE(value)) {
        return static_cast<float>(g_value_get_double(value));
    }
    return std::nullopt;
}

guint spectrumValueCount(const GValue* value)
{
    return GST_VALUE_HOLDS_ARRAY(value)
        ? gst_value_array_get_size(value)
        : gst_value_list_get_size(value);
}

const GValue* spectrumValueAt(const GValue* value, guint index)
{
    return GST_VALUE_HOLDS_ARRAY(value)
        ? gst_value_array_get_value(value, index)
        : gst_value_list_get_value(value, index);
}

std::vector<float> spectrumChannel(const GValue* values)
{
    std::vector<float> result;
    if (!GST_VALUE_HOLDS_LIST(values) && !GST_VALUE_HOLDS_ARRAY(values)) {
        return result;
    }

    const guint size = spectrumValueCount(values);
    result.reserve(size);
    for (guint index = 0; index < size; ++index) {
        if (const auto magnitude = spectrumMagnitude(spectrumValueAt(values, index))) {
            result.push_back(*magnitude);
        }
    }
    return result;
}

std::optional<AudioPlayer::SpectrumFrame> parseSpectrumMessage(GstMessage* message)
{
    const GstStructure* structure = gst_message_get_structure(message);
    if (structure == nullptr || !gst_structure_has_name(structure, "spectrum")) {
        return std::nullopt;
    }

    const GValue* magnitudes = gst_structure_get_value(structure, "magnitude");
    if (magnitudes == nullptr || (!GST_VALUE_HOLDS_LIST(magnitudes) && !GST_VALUE_HOLDS_ARRAY(magnitudes))) {
        return std::nullopt;
    }

    AudioPlayer::SpectrumFrame frame;
    const guint size = spectrumValueCount(magnitudes);
    if (size > 0) {
        const GValue* first = spectrumValueAt(magnitudes, 0);
        if (GST_VALUE_HOLDS_LIST(first) || GST_VALUE_HOLDS_ARRAY(first)) {
            frame.channelMagnitudesDb.reserve(size);
            for (guint index = 0; index < size; ++index) {
                std::vector<float> channel = spectrumChannel(spectrumValueAt(magnitudes, index));
                if (!channel.empty()) {
                    frame.channelMagnitudesDb.push_back(std::move(channel));
                }
            }
        } else {
            std::vector<float> channel = spectrumChannel(magnitudes);
            if (!channel.empty()) {
                frame.channelMagnitudesDb.push_back(std::move(channel));
            }
        }
    }

    return frame.channelMagnitudesDb.empty() ? std::nullopt : std::optional{std::move(frame)};
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

    if (GstElement* spectrum = gst_element_factory_make("spectrum", "audio-spectrum")) {
        g_object_set(
            spectrum,
            "bands", 10u,
            "multi-channel", TRUE,
            "interval", static_cast<guint64>(50 * GST_MSECOND),
            "threshold", -70,
            "message-magnitude", TRUE,
            "message-phase", FALSE,
            nullptr);
        g_object_set(player_, "audio-filter", spectrum, nullptr);
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
    std::optional<SpectrumFrame> latestSpectrum;
    GstBus* bus = gst_element_get_bus(player_);
    while (GstMessage* message = gst_bus_pop_filtered(
               bus,
               static_cast<GstMessageType>(
                   GST_MESSAGE_EOS | GST_MESSAGE_ERROR | GST_MESSAGE_STATE_CHANGED | GST_MESSAGE_ELEMENT))) {
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
        } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ELEMENT) {
            if (auto frame = parseSpectrumMessage(message)) {
                latestSpectrum = std::move(*frame);
            }
        }
        gst_message_unref(message);
    }
    gst_object_unref(bus);
    if (latestSpectrum) {
        events.push_back({
            .type = EventType::Spectrum,
            .spectrum = std::move(*latestSpectrum),
        });
    }
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
