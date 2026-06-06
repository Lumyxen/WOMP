#include "womp/library/ImportService.h"
#include "womp/mpris/MprisService.h"
#include "womp/playback/AudioPlayer.h"
#include "womp/playback/PlaybackQueue.h"
#include "womp/playback/PlaybackStats.h"
#include "womp/scene/Primitive.h"

#include <gst/gst.h>
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace womp;

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error("check failed: " #condition); } while (false)

class TemporaryDirectory {
public:
    TemporaryDirectory()
    {
        path_ = std::filesystem::temp_directory_path()
            / ("womp-test-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path_);
    }
    ~TemporaryDirectory() { std::filesystem::remove_all(path_); }
    const std::filesystem::path& path() const { return path_; }
private:
    std::filesystem::path path_;
};

void writeLe16(std::ofstream& file, std::uint16_t value)
{
    file.put(static_cast<char>(value & 0xff));
    file.put(static_cast<char>((value >> 8) & 0xff));
}

void writeLe32(std::ofstream& file, std::uint32_t value)
{
    writeLe16(file, static_cast<std::uint16_t>(value & 0xffff));
    writeLe16(file, static_cast<std::uint16_t>(value >> 16));
}

void writeWav(const std::filesystem::path& path, int durationMs)
{
    constexpr std::uint32_t sampleRate = 8000;
    const std::uint32_t samples = sampleRate * static_cast<std::uint32_t>(durationMs) / 1000;
    const std::uint32_t dataSize = samples * 2;
    std::ofstream file(path, std::ios::binary);
    file.write("RIFF", 4);
    writeLe32(file, 36 + dataSize);
    file.write("WAVEfmt ", 8);
    writeLe32(file, 16);
    writeLe16(file, 1);
    writeLe16(file, 1);
    writeLe32(file, sampleRate);
    writeLe32(file, sampleRate * 2);
    writeLe16(file, 2);
    writeLe16(file, 16);
    file.write("data", 4);
    writeLe32(file, dataSize);
    for (std::uint32_t index = 0; index < samples; ++index) {
        const auto value = static_cast<std::int16_t>(
            std::sin(static_cast<double>(index) * 0.12) * 12000.0);
        writeLe16(file, static_cast<std::uint16_t>(value));
    }
}

void writeJpegCover(const std::filesystem::path& path)
{
    gst_init(nullptr, nullptr);
    const std::string description = "videotestsrc num-buffers=1 pattern=blue ! "
        "video/x-raw,width=320,height=180 ! jpegenc ! filesink location=" + path.string();
    GError* error = nullptr;
    GstElement* pipeline = gst_parse_launch(description.c_str(), &error);
    CHECK(pipeline != nullptr);
    CHECK(error == nullptr);
    CHECK(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE);
    GstBus* bus = gst_element_get_bus(pipeline);
    GstMessage* message = gst_bus_timed_pop_filtered(
        bus, 10 * GST_SECOND, static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    CHECK(message != nullptr);
    CHECK(GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS);
    gst_message_unref(message);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}

void writeFlac(const std::filesystem::path& path, int frequency)
{
    gst_init(nullptr, nullptr);
    const std::string description = "audiotestsrc num-buffers=2 samplesperbuffer=512 freq="
        + std::to_string(frequency)
        + " ! audio/x-raw,format=S16LE,rate=8000,channels=1 ! flacenc ! filesink location="
        + path.string();
    GError* error = nullptr;
    GstElement* pipeline = gst_parse_launch(description.c_str(), &error);
    CHECK(pipeline != nullptr);
    CHECK(error == nullptr);
    CHECK(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE);
    GstBus* bus = gst_element_get_bus(pipeline);
    GstMessage* message = gst_bus_timed_pop_filtered(
        bus, 10 * GST_SECOND, static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    CHECK(message != nullptr);
    CHECK(GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS);
    gst_message_unref(message);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}

std::vector<char> bytes(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void testStoreAndImport()
{
    TemporaryDirectory temporary;
    const auto source = temporary.path() / "source";
    const auto library = temporary.path() / "library";
    std::filesystem::create_directories(source);
    const auto song = source / "Fallback Title.wav";
    const auto duplicateSong = source / "Second Location.wav";
    writeWav(song, 1000);
    std::filesystem::copy_file(song, duplicateSong);
    writeJpegCover(source / "CoVeR.jpg");
    const auto original = bytes(song);

    TrackId id;
    PlaylistId playlistId = 0;
    {
        LibraryStore store(library);
        ImportService importer(store);
        CHECK(store.loadPlaylists().empty());
        playlistId = store.createPlaylist("Test");
        const ImportResult first = importer.importFiles({song, duplicateSong}, {playlistId});
        CHECK(first.imported == 1);
        CHECK(first.duplicates == 1);
        CHECK(first.trackIds.size() == 2);
        id = first.trackIds.front();
        CHECK(id.size() == 64);
        CHECK(bytes(song) == original);
        CHECK(bytes(duplicateSong) == original);
        CHECK(std::filesystem::is_regular_file(library / "tracks" / id));

        const ImportResult duplicate = importer.importFiles({song}, {playlistId});
        CHECK(duplicate.imported == 0);
        CHECK(duplicate.duplicates == 1);
        CHECK(std::ranges::distance(std::filesystem::directory_iterator(library / "tracks"), std::filesystem::directory_iterator{}) == 1);

        const auto tracks = store.loadTracks();
        CHECK(tracks.size() == 1);
        CHECK(tracks.front().title == "Fallback Title");
        CHECK(tracks.front().album == "Unknown Album");
        CHECK(tracks.front().artists == std::vector<std::string>{"Unknown Artist"});
        CHECK(tracks.front().durationMs >= 900);
        CHECK(tracks.front().formatLabel == "WAV");
        CHECK(std::filesystem::is_regular_file(library / tracks.front().artworkRelativePath));
        CHECK(store.loadPlaylists().back().trackIds == std::vector<TrackId>{id});
        CHECK(!store.addTrackToPlaylist(9999, id));

        store.applyStatisticDeltas({{
            .trackId = id,
            .playlistId = playlistId,
            .listenedMs = 1234,
            .listenCount = 1,
            .lastPlayedAtMs = 42,
        }});
        CHECK(store.summary(std::nullopt).listenedMs == 1234);
        CHECK(store.summary(playlistId).listenedMs == 1234);
        CHECK(store.removeTrackFromPlaylist(playlistId, id));
        CHECK(store.summary(playlistId).listenedMs == 0);
        CHECK(store.summary(std::nullopt).listenedMs == 1234);

        std::filesystem::remove(library / "tracks" / id);
        CHECK(!store.findTrack(id)->available);
        const ImportResult restored = importer.importFiles({song});
        CHECK(restored.duplicates == 1);
        CHECK(store.findTrack(id)->available);
    }

    {
        LibraryStore store(library);
        CHECK(store.loadTracks().size() == 1);
        CHECK(store.findTrack(id)->available);
        CHECK(store.removeTrackFromLibrary(id));
        CHECK(!std::filesystem::exists(library / "tracks" / id));
    }
}

void testM3u()
{
    TemporaryDirectory temporary;
    const auto songA = temporary.path() / "a.wav";
    const auto songB = temporary.path() / "b.wav";
    writeWav(songA, 300);
    writeWav(songB, 400);
    std::ofstream playlist(temporary.path() / "ordered.m3u8");
    playlist << "#EXTM3U\nb.wav\nhttps://example.com/x.mp3\na.wav\n";
    playlist.close();

    LibraryStore store(temporary.path() / "library");
    ImportService importer(store);
    const PlaylistId id = store.createPlaylist("Ordered");
    const ImportResult result = importer.importM3u(temporary.path() / "ordered.m3u8", id);
    CHECK(result.imported == 2);
    const auto loaded = store.loadPlaylists();
    const auto found = std::ranges::find(loaded, id, &PlaylistRecord::id);
    CHECK(found != loaded.end());
    CHECK(found->trackIds == result.trackIds);
}

void testLargeFlacImport()
{
    TemporaryDirectory temporary;
    const auto source = temporary.path() / "source";
    const auto library = temporary.path() / "library";
    std::filesystem::create_directories(source);
    writeJpegCover(source / "cover.jpg");

    std::vector<std::filesystem::path> songs;
    for (int index = 0; index < 120; ++index) {
        const auto song = source / ("song-" + std::to_string(index) + ".flac");
        writeFlac(song, 220 + index);
        songs.push_back(song);
    }

    LibraryStore store(library);
    ImportService importer(store);
    const ImportResult result = importer.importFiles(std::move(songs));
    CHECK(result.imported == 120);
    CHECK(result.failed == 0);
    CHECK(result.unsupported == 0);
    const auto tracks = store.loadTracks();
    CHECK(tracks.size() == 120);
    CHECK(std::ranges::all_of(tracks, [](const TrackRecord& track) {
        return track.formatLabel == "FLAC" && !track.artworkRelativePath.empty();
    }));
    CHECK(std::ranges::distance(
        std::filesystem::directory_iterator(library / "artwork"),
        std::filesystem::directory_iterator{}) == 1);
}

void testQueue()
{
    PlaybackQueue ordered(1);
    ordered.start({"a", "b", "c"}, 10, "b", false);
    CHECK(ordered.current() == "b");
    CHECK(ordered.next() == "c");
    CHECK(!ordered.next());
    CHECK(ordered.previous() == "c");

    PlaybackQueue shuffledA(55);
    PlaybackQueue shuffledB(55);
    shuffledA.start({"a", "b", "c", "d"}, std::nullopt, "a", true);
    shuffledB.start({"a", "b", "c", "d"}, std::nullopt, "a", true);
    CHECK(shuffledA.upcoming() == shuffledB.upcoming());
    CHECK(shuffledA.next().has_value());
    CHECK(shuffledA.previous() == "a");
    shuffledA.setShuffle(false);
    CHECK(shuffledA.upcoming() == std::vector<TrackId>({"b", "c", "d"}));
}

void testStatistics()
{
    using Clock = PlaybackStats::Clock;
    const auto start = Clock::time_point{};
    PlaybackStats stats;
    stats.start("track", 7, 60'000, 100, start);
    stats.tick(true, start);
    stats.tick(true, start + std::chrono::seconds{20});
    auto deltas = stats.takeDeltas(start + std::chrono::seconds{20});
    CHECK(deltas.front().listenedMs == 20'000);
    CHECK(deltas.front().listenCount == 1);
    CHECK(deltas.front().playlistId == 7);
    stats.finish(false, true, start + std::chrono::seconds{21});
    deltas = stats.takeDeltas(start + std::chrono::seconds{21});
    CHECK(deltas.front().skipCount == 0);

    PlaybackStats skipped;
    skipped.start("short", std::nullopt, 10'000, 0, start);
    skipped.tick(true, start);
    skipped.finish(false, true, start + std::chrono::seconds{5});
    CHECK(skipped.takeDeltas(start + std::chrono::seconds{5}).front().skipCount == 1);

    PlaybackStats shortTrack;
    shortTrack.start("short", std::nullopt, 10'000, 0, start);
    shortTrack.tick(true, start);
    shortTrack.finish(true, false, start + std::chrono::seconds{10});
    CHECK(shortTrack.takeDeltas(start + std::chrono::seconds{10}).front().listenCount == 1);

    PlaybackStats paused;
    paused.start("paused", std::nullopt, 60'000, 0, start);
    paused.tick(true, start);
    paused.pause(start + std::chrono::seconds{5});
    paused.tick(false, start + std::chrono::seconds{30});
    paused.finish(false, false, start + std::chrono::seconds{30});
    CHECK(paused.takeDeltas(start + std::chrono::seconds{30}).front().listenedMs == 5000);
}

void testLegacyAndMalformed()
{
    TemporaryDirectory temporary;
    const auto legacy = temporary.path() / "legacy";
    std::filesystem::create_directories(legacy);
    {
        std::ofstream state(legacy / "state.conf");
        state << "lastImportDirectory=/tmp/music\n";
    }
    LibraryStore migrated(legacy);
    CHECK(migrated.setting("last_import_directory") == "/tmp/music");

    const auto malformed = temporary.path() / "malformed";
    std::filesystem::create_directories(malformed);
    {
        std::ofstream database(malformed / "library.sqlite3");
        database << "not a sqlite database";
    }
    bool threw = false;
    try {
        LibraryStore invalid(malformed);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

void testLegacyDefaultPlaylistMigration()
{
    TemporaryDirectory temporary;
    const auto library = temporary.path() / "library";
    std::filesystem::create_directories(library);
    sqlite3* database = nullptr;
    CHECK(sqlite3_open((library / "library.sqlite3").c_str(), &database) == SQLITE_OK);
    CHECK(sqlite3_exec(database, R"SQL(
        CREATE TABLE playlists(id INTEGER PRIMARY KEY, name TEXT NOT NULL, created_at_ms INTEGER NOT NULL, position INTEGER NOT NULL);
        CREATE TABLE playlist_tracks(playlist_id INTEGER NOT NULL, track_id TEXT NOT NULL, position INTEGER NOT NULL);
        CREATE TABLE tracks(id TEXT PRIMARY KEY, format_label TEXT NOT NULL);
        INSERT INTO playlists VALUES(1, 'Recently Added', 0, 0);
        INSERT INTO playlists VALUES(2, 'Favorites', 0, 1);
        INSERT INTO playlists VALUES(3, 'Custom', 0, 2);
        INSERT INTO playlist_tracks VALUES(2, 'kept-track', 0);
        INSERT INTO tracks VALUES('flac-track', 'audio/x-flac');
        INSERT INTO tracks VALUES('mp3-track', 'application/x-id3');
        PRAGMA user_version=1;
    )SQL", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(database);

    LibraryStore migrated(library);
    const auto playlists = migrated.loadPlaylists();
    CHECK(playlists.size() == 1);
    CHECK(playlists.front().name == "Custom");
    sqlite3* migratedDatabase = nullptr;
    CHECK(sqlite3_open((library / "library.sqlite3").c_str(), &migratedDatabase) == SQLITE_OK);
    sqlite3_stmt* statement = nullptr;
    CHECK(sqlite3_prepare_v2(
        migratedDatabase,
        "SELECT GROUP_CONCAT(format_label, ',') FROM tracks ORDER BY id",
        -1,
        &statement,
        nullptr) == SQLITE_OK);
    CHECK(sqlite3_step(statement) == SQLITE_ROW);
    CHECK(std::string(reinterpret_cast<const char*>(sqlite3_column_text(statement, 0))) == "FLAC,MP3");
    sqlite3_finalize(statement);
    sqlite3_close(migratedDatabase);
}

void testAudioAndPrimitive()
{
    TemporaryDirectory temporary;
    const auto song = temporary.path() / "test.wav";
    writeWav(song, 200);
    AudioPlayer player(true);
    player.setVolume(0.5);
    for (int index = 0; index < 20; ++index) {
        CHECK(player.play(song));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    CHECK(std::ranges::none_of(player.pollEvents(), [](const AudioPlayer::Event& event) {
        return event.type == AudioPlayer::EventType::Error;
    }));
    CHECK(player.durationMs() >= 150);
    CHECK(player.seek(50));
    player.pause();

    Primitive image = Primitive::image({
        .x = 1,
        .y = 2,
        .width = 64,
        .height = 64,
        .source = "cover.png",
    });
    CHECK(std::get<ImagePrimitive>(image.geometry).source == "cover.png");

    PrimitiveStore primitives;
    const PrimitiveId first = primitives.add(Primitive::quad({.width = 1}));
    const PrimitiveId second = primitives.add(Primitive::quad({.width = 2}));
    CHECK(std::get<QuadPrimitive>(primitives.find(first)->geometry).width == 1);
    CHECK(primitives.remove(first));
    CHECK(primitives.find(first) == nullptr);
    CHECK(std::get<QuadPrimitive>(primitives.find(second)->geometry).width == 2);

    MprisService mpris;
    mpris.update("Paused", 50'000, 0.5, true, std::nullopt);
    CHECK(mpris.takeCommands().empty());
}

} // namespace

int main()
{
    try {
        testStoreAndImport();
        testM3u();
        testLargeFlacImport();
        testQueue();
        testStatistics();
        testLegacyAndMalformed();
        testLegacyDefaultPlaylistMigration();
        testAudioAndPrimitive();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
