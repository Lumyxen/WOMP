#pragma once

#include "womp/library/Models.h"

#include <gio/gio.h>

#include <cstdint>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace womp {

class MprisService {
public:
    enum class CommandType {
        Play,
        Pause,
        PlayPause,
        Next,
        Previous,
        Seek,
        SetPosition,
        SetShuffle,
        SetVolume,
    };

    struct Command {
        CommandType type = CommandType::PlayPause;
        std::int64_t positionUs = 0;
        bool boolean = false;
        double number = 0.0;
    };

    struct Metadata {
        TrackId trackId;
        std::string title;
        std::string album;
        std::vector<std::string> artists;
        std::int64_t durationUs = 0;
        std::filesystem::path artworkPath;
    };

    MprisService();
    ~MprisService();

    MprisService(const MprisService&) = delete;
    MprisService& operator=(const MprisService&) = delete;

    int wakeFd() const { return wakeFd_; }
    std::vector<Command> takeCommands();
    void update(
        std::string playbackStatus,
        std::int64_t positionUs,
        double volume,
        bool shuffle,
        std::optional<Metadata> metadata);
    void updatePosition(std::int64_t positionUs);
    void emitSeeked(std::int64_t positionUs);

private:
    void run();
    void enqueue(Command command);
    void emitPropertiesChanged();

    static void methodCall(
        GDBusConnection* connection,
        const char* sender,
        const char* objectPath,
        const char* interfaceName,
        const char* methodName,
        GVariant* parameters,
        GDBusMethodInvocation* invocation,
        void* userData);
    static GVariant* getProperty(
        GDBusConnection* connection,
        const char* sender,
        const char* objectPath,
        const char* interfaceName,
        const char* propertyName,
        GError** error,
        void* userData);
    static gboolean setProperty(
        GDBusConnection* connection,
        const char* sender,
        const char* objectPath,
        const char* interfaceName,
        const char* propertyName,
        GVariant* value,
        GError** error,
        void* userData);

    mutable std::mutex mutex_;
    std::deque<Command> commands_;
    std::string playbackStatus_ = "Stopped";
    std::int64_t positionUs_ = 0;
    double volume_ = 1.0;
    bool shuffle_ = false;
    std::optional<Metadata> metadata_;
    int wakeFd_ = -1;
    GMainContext* context_ = nullptr;
    GMainLoop* loop_ = nullptr;
    GDBusConnection* connection_ = nullptr;
    std::thread thread_;
    std::atomic_bool stopping_ = false;
    std::mutex lifecycleMutex_;
    std::condition_variable lifecycleCondition_;
    bool initialized_ = false;
};

} // namespace womp
