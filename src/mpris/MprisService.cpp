#include "womp/mpris/MprisService.h"

#include <gio/gio.h>

#include <sys/eventfd.h>
#include <unistd.h>

#include <array>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace womp {

namespace {

constexpr const char* introspectionXml = R"XML(
<node>
  <interface name="org.mpris.MediaPlayer2">
    <method name="Raise"/>
    <method name="Quit"/>
    <property name="CanQuit" type="b" access="read"/>
    <property name="CanRaise" type="b" access="read"/>
    <property name="HasTrackList" type="b" access="read"/>
    <property name="Identity" type="s" access="read"/>
    <property name="DesktopEntry" type="s" access="read"/>
    <property name="SupportedUriSchemes" type="as" access="read"/>
    <property name="SupportedMimeTypes" type="as" access="read"/>
  </interface>
  <interface name="org.mpris.MediaPlayer2.Player">
    <method name="Next"/><method name="Previous"/><method name="Pause"/><method name="PlayPause"/><method name="Stop"/><method name="Play"/>
    <method name="Seek"><arg direction="in" type="x" name="Offset"/></method>
    <method name="SetPosition"><arg direction="in" type="o" name="TrackId"/><arg direction="in" type="x" name="Position"/></method>
    <signal name="Seeked"><arg type="x" name="Position"/></signal>
    <property name="PlaybackStatus" type="s" access="read"/>
    <property name="Metadata" type="a{sv}" access="read"/>
    <property name="Volume" type="d" access="readwrite"/>
    <property name="Position" type="x" access="read"/>
    <property name="MinimumRate" type="d" access="read"/>
    <property name="MaximumRate" type="d" access="read"/>
    <property name="CanGoNext" type="b" access="read"/>
    <property name="CanGoPrevious" type="b" access="read"/>
    <property name="CanPlay" type="b" access="read"/>
    <property name="CanPause" type="b" access="read"/>
    <property name="CanSeek" type="b" access="read"/>
    <property name="CanControl" type="b" access="read"/>
    <property name="Shuffle" type="b" access="readwrite"/>
  </interface>
</node>
)XML";

GVariant* stringArray(const std::vector<std::string>& values)
{
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));
    for (const std::string& value : values) {
        g_variant_builder_add(&builder, "s", value.c_str());
    }
    return g_variant_builder_end(&builder);
}

std::string trackObjectPath(const TrackId& id)
{
    return id.empty() ? "/org/mpris/MediaPlayer2/track/none" : "/org/mpris/MediaPlayer2/track/" + id;
}

} // namespace

MprisService::MprisService()
{
    wakeFd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeFd_ < 0) {
        throw std::runtime_error("failed to create MPRIS wake eventfd");
    }
    thread_ = std::thread([this] { run(); });
    std::unique_lock lock(lifecycleMutex_);
    lifecycleCondition_.wait(lock, [this] { return initialized_; });
}

MprisService::~MprisService()
{
    stopping_ = true;
    if (context_ != nullptr && loop_ != nullptr) {
        g_main_context_invoke(context_, [](gpointer data) -> gboolean {
            g_main_loop_quit(static_cast<GMainLoop*>(data));
            return G_SOURCE_REMOVE;
        }, loop_);
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    close(wakeFd_);
}

void MprisService::run()
{
    context_ = g_main_context_new();
    g_main_context_push_thread_default(context_);
    loop_ = g_main_loop_new(context_, FALSE);
    {
        std::scoped_lock lock(lifecycleMutex_);
        initialized_ = true;
    }
    lifecycleCondition_.notify_one();
    if (stopping_) {
        g_main_loop_unref(loop_);
        loop_ = nullptr;
        g_main_context_pop_thread_default(context_);
        g_main_context_unref(context_);
        context_ = nullptr;
        return;
    }

    GError* error = nullptr;
    connection_ = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (connection_ != nullptr) {
        GDBusNodeInfo* node = g_dbus_node_info_new_for_xml(introspectionXml, &error);
        static const GDBusInterfaceVTable vtable{
            .method_call = methodCall,
            .get_property = getProperty,
            .set_property = setProperty,
        };
        for (guint index = 0; node != nullptr && node->interfaces[index] != nullptr; ++index) {
            g_dbus_connection_register_object(
                connection_,
                "/org/mpris/MediaPlayer2",
                node->interfaces[index],
                &vtable,
                this,
                nullptr,
                nullptr);
        }
        g_bus_own_name_on_connection(
            connection_,
            "org.mpris.MediaPlayer2.womp",
            G_BUS_NAME_OWNER_FLAGS_NONE,
            nullptr,
            nullptr,
            nullptr,
            nullptr);
        if (node != nullptr) g_dbus_node_info_unref(node);
        if (!stopping_) {
            g_main_loop_run(loop_);
        }
        g_object_unref(connection_);
        connection_ = nullptr;
    }
    g_clear_error(&error);
    g_main_loop_unref(loop_);
    loop_ = nullptr;
    g_main_context_pop_thread_default(context_);
    g_main_context_unref(context_);
    context_ = nullptr;
}

void MprisService::enqueue(Command command)
{
    {
        std::scoped_lock lock(mutex_);
        commands_.push_back(std::move(command));
    }
    const std::uint64_t one = 1;
    write(wakeFd_, &one, sizeof(one));
}

std::vector<MprisService::Command> MprisService::takeCommands()
{
    std::uint64_t count = 0;
    while (read(wakeFd_, &count, sizeof(count)) > 0) {
    }
    std::scoped_lock lock(mutex_);
    std::vector<Command> commands(commands_.begin(), commands_.end());
    commands_.clear();
    return commands;
}

void MprisService::update(
    std::string playbackStatus,
    std::int64_t positionUs,
    double volume,
    bool shuffle,
    std::optional<Metadata> metadata)
{
    {
        std::scoped_lock lock(mutex_);
        playbackStatus_ = std::move(playbackStatus);
        positionUs_ = positionUs;
        volume_ = volume;
        shuffle_ = shuffle;
        metadata_ = std::move(metadata);
    }
    if (context_ != nullptr) {
        g_main_context_invoke(context_, [](gpointer data) -> gboolean {
            static_cast<MprisService*>(data)->emitPropertiesChanged();
            return G_SOURCE_REMOVE;
        }, this);
    }
}

void MprisService::updatePosition(std::int64_t positionUs)
{
    std::scoped_lock lock(mutex_);
    positionUs_ = positionUs;
}

void MprisService::emitPropertiesChanged()
{
    if (connection_ == nullptr) {
        return;
    }
    GVariantBuilder changed;
    GVariantBuilder invalidated;
    g_variant_builder_init(&changed, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_init(&invalidated, G_VARIANT_TYPE("as"));
    g_variant_builder_add(&changed, "{sv}", "PlaybackStatus", getProperty(connection_, nullptr, nullptr, "org.mpris.MediaPlayer2.Player", "PlaybackStatus", nullptr, this));
    g_variant_builder_add(&changed, "{sv}", "Metadata", getProperty(connection_, nullptr, nullptr, "org.mpris.MediaPlayer2.Player", "Metadata", nullptr, this));
    g_variant_builder_add(&changed, "{sv}", "Volume", getProperty(connection_, nullptr, nullptr, "org.mpris.MediaPlayer2.Player", "Volume", nullptr, this));
    g_variant_builder_add(&changed, "{sv}", "Shuffle", getProperty(connection_, nullptr, nullptr, "org.mpris.MediaPlayer2.Player", "Shuffle", nullptr, this));
    g_dbus_connection_emit_signal(
        connection_, nullptr, "/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties", "PropertiesChanged",
        g_variant_new("(sa{sv}as)", "org.mpris.MediaPlayer2.Player", &changed, &invalidated), nullptr);
}

void MprisService::emitSeeked(std::int64_t positionUs)
{
    if (context_ == nullptr) return;
    g_main_context_invoke(context_, [](gpointer data) -> gboolean {
        auto* pair = static_cast<std::pair<MprisService*, std::int64_t>*>(data);
        if (pair->first->connection_ != nullptr) {
            g_dbus_connection_emit_signal(
                pair->first->connection_, nullptr, "/org/mpris/MediaPlayer2",
                "org.mpris.MediaPlayer2.Player", "Seeked", g_variant_new("(x)", pair->second), nullptr);
        }
        delete pair;
        return G_SOURCE_REMOVE;
    }, new std::pair<MprisService*, std::int64_t>{this, positionUs});
}

void MprisService::methodCall(
    GDBusConnection*, const char*, const char*, const char*, const char* methodName,
    GVariant* parameters, GDBusMethodInvocation* invocation, void* userData)
{
    auto& service = *static_cast<MprisService*>(userData);
    if (std::strcmp(methodName, "Play") == 0) service.enqueue({.type = CommandType::Play});
    else if (std::strcmp(methodName, "Pause") == 0) service.enqueue({.type = CommandType::Pause});
    else if (std::strcmp(methodName, "PlayPause") == 0) service.enqueue({.type = CommandType::PlayPause});
    else if (std::strcmp(methodName, "Next") == 0) service.enqueue({.type = CommandType::Next});
    else if (std::strcmp(methodName, "Previous") == 0) service.enqueue({.type = CommandType::Previous});
    else if (std::strcmp(methodName, "Seek") == 0) {
        gint64 offset = 0;
        g_variant_get(parameters, "(x)", &offset);
        service.enqueue({.type = CommandType::Seek, .positionUs = offset});
    } else if (std::strcmp(methodName, "SetPosition") == 0) {
        const gchar* track = nullptr;
        gint64 position = 0;
        g_variant_get(parameters, "(&ox)", &track, &position);
        service.enqueue({.type = CommandType::SetPosition, .positionUs = position});
    }
    g_dbus_method_invocation_return_value(invocation, nullptr);
}

GVariant* MprisService::getProperty(
    GDBusConnection*, const char*, const char*, const char* interfaceName,
    const char* propertyName, GError**, void* userData)
{
    auto& service = *static_cast<MprisService*>(userData);
    std::scoped_lock lock(service.mutex_);
    if (std::strcmp(interfaceName, "org.mpris.MediaPlayer2") == 0) {
        if (std::strcmp(propertyName, "CanQuit") == 0 || std::strcmp(propertyName, "CanRaise") == 0 || std::strcmp(propertyName, "HasTrackList") == 0) return g_variant_new_boolean(FALSE);
        if (std::strcmp(propertyName, "Identity") == 0 || std::strcmp(propertyName, "DesktopEntry") == 0) return g_variant_new_string("womp");
        if (std::strcmp(propertyName, "SupportedUriSchemes") == 0 || std::strcmp(propertyName, "SupportedMimeTypes") == 0) return stringArray({});
    }
    if (std::strcmp(propertyName, "PlaybackStatus") == 0) return g_variant_new_string(service.playbackStatus_.c_str());
    if (std::strcmp(propertyName, "Volume") == 0) return g_variant_new_double(service.volume_);
    if (std::strcmp(propertyName, "Shuffle") == 0) return g_variant_new_boolean(service.shuffle_);
    if (std::strcmp(propertyName, "Position") == 0) return g_variant_new_int64(service.positionUs_);
    if (std::strcmp(propertyName, "MinimumRate") == 0 || std::strcmp(propertyName, "MaximumRate") == 0) return g_variant_new_double(1.0);
    if (std::strncmp(propertyName, "Can", 3) == 0) return g_variant_new_boolean(TRUE);
    if (std::strcmp(propertyName, "Metadata") == 0) {
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
        if (service.metadata_) {
            const Metadata& metadata = *service.metadata_;
            g_variant_builder_add(&builder, "{sv}", "mpris:trackid", g_variant_new_object_path(trackObjectPath(metadata.trackId).c_str()));
            g_variant_builder_add(&builder, "{sv}", "mpris:length", g_variant_new_int64(metadata.durationUs));
            g_variant_builder_add(&builder, "{sv}", "xesam:title", g_variant_new_string(metadata.title.c_str()));
            g_variant_builder_add(&builder, "{sv}", "xesam:album", g_variant_new_string(metadata.album.c_str()));
            g_variant_builder_add(&builder, "{sv}", "xesam:artist", stringArray(metadata.artists));
            if (!metadata.artworkPath.empty()) {
                gchar* uri = g_filename_to_uri(metadata.artworkPath.c_str(), nullptr, nullptr);
                if (uri != nullptr) {
                    g_variant_builder_add(&builder, "{sv}", "mpris:artUrl", g_variant_new_string(uri));
                    g_free(uri);
                }
            }
        }
        return g_variant_builder_end(&builder);
    }
    return nullptr;
}

gboolean MprisService::setProperty(
    GDBusConnection*, const char*, const char*, const char*, const char* propertyName,
    GVariant* value, GError**, void* userData)
{
    auto& service = *static_cast<MprisService*>(userData);
    if (std::strcmp(propertyName, "Volume") == 0) {
        service.enqueue({.type = CommandType::SetVolume, .number = g_variant_get_double(value)});
        return TRUE;
    }
    if (std::strcmp(propertyName, "Shuffle") == 0) {
        service.enqueue({.type = CommandType::SetShuffle, .boolean = static_cast<bool>(g_variant_get_boolean(value))});
        return TRUE;
    }
    return FALSE;
}

} // namespace womp
