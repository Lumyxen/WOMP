#include "womp/library/LibraryStore.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>

namespace womp {

namespace {

constexpr int schemaVersion = 2;

std::int64_t unixTimeMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

class Statement {
public:
    Statement(sqlite3* database, std::string_view sql)
    {
        if (sqlite3_prepare_v2(database, sql.data(), static_cast<int>(sql.size()), &statement_, nullptr) != SQLITE_OK) {
            throw std::runtime_error("SQLite prepare failed: " + std::string(sqlite3_errmsg(database)));
        }
    }

    ~Statement() { sqlite3_finalize(statement_); }
    sqlite3_stmt* get() const { return statement_; }

private:
    sqlite3_stmt* statement_ = nullptr;
};

class Transaction {
public:
    explicit Transaction(sqlite3* database)
        : database_(database)
    {
        if (sqlite3_exec(database_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) {
            throw std::runtime_error("SQLite transaction failed: " + std::string(sqlite3_errmsg(database_)));
        }
    }

    ~Transaction()
    {
        if (!committed_) {
            sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr);
        }
    }

    void commit()
    {
        if (sqlite3_exec(database_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
            throw std::runtime_error("SQLite commit failed: " + std::string(sqlite3_errmsg(database_)));
        }
        committed_ = true;
    }

private:
    sqlite3* database_;
    bool committed_ = false;
};

void bindText(sqlite3_stmt* statement, int index, std::string_view value)
{
    sqlite3_bind_text(statement, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT);
}

std::string columnText(sqlite3_stmt* statement, int index)
{
    const auto* text = sqlite3_column_text(statement, index);
    return text == nullptr ? std::string{} : reinterpret_cast<const char*>(text);
}

void requireDone(sqlite3* database, sqlite3_stmt* statement)
{
    if (sqlite3_step(statement) != SQLITE_DONE) {
        throw std::runtime_error("SQLite write failed: " + std::string(sqlite3_errmsg(database)));
    }
}

std::filesystem::path homeDirectory()
{
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return home;
    }
    return std::filesystem::current_path();
}

} // namespace

LibraryStore::LibraryStore(std::filesystem::path dataDirectory)
    : dataDirectory_(std::move(dataDirectory))
{
    std::error_code error;
    std::filesystem::create_directories(dataDirectory_ / "tracks", error);
    std::filesystem::create_directories(dataDirectory_ / "artwork", error);
    std::filesystem::create_directories(dataDirectory_ / "tmp", error);
    if (error) {
        throw std::runtime_error("failed to create library directory: " + error.message());
    }

    for (const auto& entry : std::filesystem::directory_iterator(dataDirectory_ / "tmp", error)) {
        if (entry.path().extension() == ".part") {
            std::filesystem::remove(entry.path(), error);
            error.clear();
        }
    }

    const std::filesystem::path databasePath = dataDirectory_ / "library.sqlite3";
    if (sqlite3_open_v2(
            databasePath.c_str(),
            &database_,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
            nullptr) != SQLITE_OK) {
        const std::string message = database_ == nullptr ? "unknown error" : sqlite3_errmsg(database_);
        sqlite3_close(database_);
        database_ = nullptr;
        throw std::runtime_error("failed to open library database: " + message);
    }

    try {
        initialize();
    } catch (...) {
        sqlite3_close(database_);
        database_ = nullptr;
        throw;
    }
}

LibraryStore::~LibraryStore()
{
    sqlite3_close(database_);
}

std::filesystem::path LibraryStore::defaultDataDirectory()
{
    if (const char* dataHome = std::getenv("XDG_DATA_HOME"); dataHome != nullptr && *dataHome != '\0') {
        return std::filesystem::path{dataHome} / "womp";
    }
    return homeDirectory() / ".local" / "share" / "womp";
}

void LibraryStore::execute(std::string_view sql) const
{
    char* error = nullptr;
    if (sqlite3_exec(database_, std::string(sql).c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
        const std::string message = error == nullptr ? sqlite3_errmsg(database_) : error;
        sqlite3_free(error);
        throw std::runtime_error("SQLite error: " + message);
    }
}

void LibraryStore::initialize()
{
    sqlite3_busy_timeout(database_, 3000);
    execute("PRAGMA foreign_keys=ON");
    execute("PRAGMA journal_mode=WAL");
    execute("PRAGMA synchronous=NORMAL");
    execute("PRAGMA quick_check");

    Statement versionStatement(database_, "PRAGMA user_version");
    if (sqlite3_step(versionStatement.get()) != SQLITE_ROW) {
        throw std::runtime_error("failed to read SQLite schema version");
    }
    const int version = sqlite3_column_int(versionStatement.get(), 0);
    if (version > schemaVersion) {
        throw std::runtime_error("library database was created by a newer womp version");
    }

    if (version == 0) {
        Transaction transaction(database_);
        execute(R"SQL(
            CREATE TABLE settings(key TEXT PRIMARY KEY, value TEXT NOT NULL);
            CREATE TABLE tracks(
                id TEXT PRIMARY KEY,
                relative_path TEXT NOT NULL UNIQUE,
                title TEXT NOT NULL,
                album TEXT NOT NULL,
                duration_ms INTEGER NOT NULL,
                format_label TEXT NOT NULL,
                artwork_relative_path TEXT NOT NULL,
                added_at_ms INTEGER NOT NULL
            );
            CREATE TABLE track_artists(
                track_id TEXT NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,
                position INTEGER NOT NULL,
                name TEXT NOT NULL,
                PRIMARY KEY(track_id, position)
            );
            CREATE TABLE track_stats(
                track_id TEXT PRIMARY KEY REFERENCES tracks(id) ON DELETE CASCADE,
                listened_ms INTEGER NOT NULL DEFAULT 0,
                listen_count INTEGER NOT NULL DEFAULT 0,
                skip_count INTEGER NOT NULL DEFAULT 0,
                last_played_at_ms INTEGER
            );
            CREATE TABLE playlists(
                id INTEGER PRIMARY KEY,
                name TEXT NOT NULL,
                created_at_ms INTEGER NOT NULL,
                position INTEGER NOT NULL
            );
            CREATE TABLE playlist_tracks(
                playlist_id INTEGER NOT NULL REFERENCES playlists(id) ON DELETE CASCADE,
                track_id TEXT NOT NULL REFERENCES tracks(id) ON DELETE CASCADE,
                position INTEGER NOT NULL,
                PRIMARY KEY(playlist_id, track_id),
                UNIQUE(playlist_id, position)
            );
            CREATE TABLE playlist_track_stats(
                playlist_id INTEGER NOT NULL,
                track_id TEXT NOT NULL,
                listened_ms INTEGER NOT NULL DEFAULT 0,
                listen_count INTEGER NOT NULL DEFAULT 0,
                skip_count INTEGER NOT NULL DEFAULT 0,
                last_played_at_ms INTEGER,
                PRIMARY KEY(playlist_id, track_id),
                FOREIGN KEY(playlist_id, track_id) REFERENCES playlist_tracks(playlist_id, track_id) ON DELETE CASCADE
            );
            PRAGMA user_version=2;
        )SQL");
        transaction.commit();
        migrateLegacyState();
    } else if (version == 1) {
        Transaction transaction(database_);
        removeLegacyDefaultPlaylists();
        execute(R"SQL(
            UPDATE tracks SET format_label=CASE LOWER(format_label)
                WHEN 'audio/x-flac' THEN 'FLAC'
                WHEN 'application/x-id3' THEN 'MP3'
                WHEN 'audio/mpeg' THEN 'MP3'
                WHEN 'audio/x-wav' THEN 'WAV'
                WHEN 'audio/x-riff' THEN 'WAV'
                WHEN 'audio/x-aiff' THEN 'AIFF'
                WHEN 'audio/x-vorbis' THEN 'OGG'
                WHEN 'application/ogg' THEN 'OGG'
                WHEN 'audio/x-opus' THEN 'OPUS'
                ELSE format_label
            END
        )SQL");
        execute("PRAGMA user_version=2");
        transaction.commit();
    }
}

void LibraryStore::removeLegacyDefaultPlaylists()
{
    execute(R"SQL(
        DELETE FROM playlists
        WHERE name IN ('Recently Added', 'Favorites', 'Long Drives')
    )SQL");
}

void LibraryStore::migrateLegacyState()
{
    std::ifstream state(dataDirectory_ / "state.conf");
    std::string line;
    while (std::getline(state, line)) {
        constexpr std::string_view prefix = "lastImportDirectory=";
        if (line.starts_with(prefix)) {
            setSetting("last_import_directory", std::string_view{line}.substr(prefix.size()));
            return;
        }
    }
}

std::filesystem::path LibraryStore::absoluteTrackPath(const TrackRecord& track) const
{
    return dataDirectory_ / track.relativePath;
}

std::filesystem::path LibraryStore::absoluteArtworkPath(const TrackRecord& track) const
{
    return track.artworkRelativePath.empty() ? std::filesystem::path{} : dataDirectory_ / track.artworkRelativePath;
}

std::vector<TrackRecord> LibraryStore::loadTracks() const
{
    std::lock_guard lock(databaseMutex_);
    std::unordered_map<TrackId, std::vector<std::string>> artistsByTrack;
    Statement artists(database_, "SELECT track_id, name FROM track_artists ORDER BY track_id, position");
    while (sqlite3_step(artists.get()) == SQLITE_ROW) {
        artistsByTrack[columnText(artists.get(), 0)].push_back(columnText(artists.get(), 1));
    }

    Statement statement(database_, R"SQL(
        SELECT t.id, t.relative_path, t.title, t.album, t.duration_ms, t.format_label,
               t.artwork_relative_path, t.added_at_ms, s.listened_ms, s.listen_count,
               s.skip_count, s.last_played_at_ms
        FROM tracks t JOIN track_stats s ON s.track_id=t.id
        ORDER BY t.added_at_ms, t.id
    )SQL");
    std::vector<TrackRecord> tracks;
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        TrackRecord track{
            .id = columnText(statement.get(), 0),
            .relativePath = columnText(statement.get(), 1),
            .title = columnText(statement.get(), 2),
            .album = columnText(statement.get(), 3),
            .durationMs = sqlite3_column_int64(statement.get(), 4),
            .formatLabel = columnText(statement.get(), 5),
            .artworkRelativePath = columnText(statement.get(), 6),
            .addedAtMs = sqlite3_column_int64(statement.get(), 7),
            .stats = {
                .listenedMs = sqlite3_column_int64(statement.get(), 8),
                .listenCount = sqlite3_column_int64(statement.get(), 9),
                .skipCount = sqlite3_column_int64(statement.get(), 10),
            },
        };
        if (sqlite3_column_type(statement.get(), 11) != SQLITE_NULL) {
            track.stats.lastPlayedAtMs = sqlite3_column_int64(statement.get(), 11);
        }
        track.available = std::filesystem::is_regular_file(dataDirectory_ / track.relativePath);
        if (auto found = artistsByTrack.find(track.id); found != artistsByTrack.end()) {
            track.artists = std::move(found->second);
        }
        tracks.push_back(std::move(track));
    }
    return tracks;
}

std::vector<TrackId> LibraryStore::loadTrackIds() const
{
    std::lock_guard lock(databaseMutex_);
    Statement statement(database_, "SELECT id FROM tracks");
    std::vector<TrackId> ids;
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        ids.push_back(columnText(statement.get(), 0));
    }
    return ids;
}

std::vector<PlaylistRecord> LibraryStore::loadPlaylists() const
{
    std::lock_guard lock(databaseMutex_);
    std::unordered_map<PlaylistId, std::vector<TrackId>> tracksByPlaylist;
    Statement tracks(database_, "SELECT playlist_id, track_id FROM playlist_tracks ORDER BY playlist_id, position");
    while (sqlite3_step(tracks.get()) == SQLITE_ROW) {
        tracksByPlaylist[sqlite3_column_int64(tracks.get(), 0)].push_back(columnText(tracks.get(), 1));
    }

    Statement statement(database_, "SELECT id, name, created_at_ms, position FROM playlists ORDER BY position, id");
    std::vector<PlaylistRecord> playlists;
    while (sqlite3_step(statement.get()) == SQLITE_ROW) {
        PlaylistRecord playlist{
            .id = sqlite3_column_int64(statement.get(), 0),
            .name = columnText(statement.get(), 1),
            .createdAtMs = sqlite3_column_int64(statement.get(), 2),
            .position = sqlite3_column_int64(statement.get(), 3),
        };
        if (auto found = tracksByPlaylist.find(playlist.id); found != tracksByPlaylist.end()) {
            playlist.trackIds = std::move(found->second);
        }
        playlists.push_back(std::move(playlist));
    }
    return playlists;
}

std::optional<TrackRecord> LibraryStore::findTrack(const TrackId& id) const
{
    std::lock_guard lock(databaseMutex_);
    Statement statement(database_, R"SQL(
        SELECT t.id, t.relative_path, t.title, t.album, t.duration_ms, t.format_label,
               t.artwork_relative_path, t.added_at_ms, s.listened_ms, s.listen_count,
               s.skip_count, s.last_played_at_ms
        FROM tracks t JOIN track_stats s ON s.track_id=t.id
        WHERE t.id=?
    )SQL");
    bindText(statement.get(), 1, id);
    if (sqlite3_step(statement.get()) != SQLITE_ROW) {
        return std::nullopt;
    }
    TrackRecord track{
        .id = columnText(statement.get(), 0),
        .relativePath = columnText(statement.get(), 1),
        .title = columnText(statement.get(), 2),
        .album = columnText(statement.get(), 3),
        .durationMs = sqlite3_column_int64(statement.get(), 4),
        .formatLabel = columnText(statement.get(), 5),
        .artworkRelativePath = columnText(statement.get(), 6),
        .addedAtMs = sqlite3_column_int64(statement.get(), 7),
        .stats = {
            .listenedMs = sqlite3_column_int64(statement.get(), 8),
            .listenCount = sqlite3_column_int64(statement.get(), 9),
            .skipCount = sqlite3_column_int64(statement.get(), 10),
        },
    };
    if (sqlite3_column_type(statement.get(), 11) != SQLITE_NULL) {
        track.stats.lastPlayedAtMs = sqlite3_column_int64(statement.get(), 11);
    }
    track.available = std::filesystem::is_regular_file(dataDirectory_ / track.relativePath);
    Statement artists(database_, "SELECT name FROM track_artists WHERE track_id=? ORDER BY position");
    bindText(artists.get(), 1, track.id);
    while (sqlite3_step(artists.get()) == SQLITE_ROW) {
        track.artists.push_back(columnText(artists.get(), 0));
    }
    return track;
}

std::optional<std::string> LibraryStore::setting(std::string_view key) const
{
    std::lock_guard lock(databaseMutex_);
    Statement statement(database_, "SELECT value FROM settings WHERE key=?");
    bindText(statement.get(), 1, key);
    return sqlite3_step(statement.get()) == SQLITE_ROW
        ? std::optional<std::string>{columnText(statement.get(), 0)}
        : std::nullopt;
}

void LibraryStore::setSetting(std::string_view key, std::string_view value)
{
    std::lock_guard lock(databaseMutex_);
    Statement statement(database_, "INSERT INTO settings(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    bindText(statement.get(), 1, key);
    bindText(statement.get(), 2, value);
    requireDone(database_, statement.get());
}

PlaylistId LibraryStore::createPlaylist(std::string_view name)
{
    std::lock_guard lock(databaseMutex_);
    Statement statement(database_, R"SQL(
        INSERT INTO playlists(name, created_at_ms, position)
        VALUES(?, ?, COALESCE((SELECT MAX(position)+1 FROM playlists), 0))
    )SQL");
    bindText(statement.get(), 1, name);
    sqlite3_bind_int64(statement.get(), 2, unixTimeMs());
    requireDone(database_, statement.get());
    return sqlite3_last_insert_rowid(database_);
}

bool LibraryStore::removePlaylist(PlaylistId id)
{
    std::lock_guard lock(databaseMutex_);
    Statement statement(database_, "DELETE FROM playlists WHERE id=?");
    sqlite3_bind_int64(statement.get(), 1, id);
    requireDone(database_, statement.get());
    return sqlite3_changes(database_) != 0;
}

bool LibraryStore::addTrackToPlaylist(PlaylistId playlistId, const TrackId& trackId)
{
    std::lock_guard lock(databaseMutex_);
    Statement statement(database_, R"SQL(
        INSERT OR IGNORE INTO playlist_tracks(playlist_id, track_id, position)
        SELECT ?, ?, COALESCE((SELECT MAX(position)+1 FROM playlist_tracks WHERE playlist_id=?), 0)
        WHERE EXISTS(SELECT 1 FROM playlists WHERE id=?)
          AND EXISTS(SELECT 1 FROM tracks WHERE id=?)
    )SQL");
    sqlite3_bind_int64(statement.get(), 1, playlistId);
    bindText(statement.get(), 2, trackId);
    sqlite3_bind_int64(statement.get(), 3, playlistId);
    sqlite3_bind_int64(statement.get(), 4, playlistId);
    bindText(statement.get(), 5, trackId);
    requireDone(database_, statement.get());
    return sqlite3_changes(database_) != 0;
}

bool LibraryStore::removeTrackFromPlaylist(PlaylistId playlistId, const TrackId& trackId)
{
    std::lock_guard lock(databaseMutex_);
    Statement statement(database_, "DELETE FROM playlist_tracks WHERE playlist_id=? AND track_id=?");
    sqlite3_bind_int64(statement.get(), 1, playlistId);
    bindText(statement.get(), 2, trackId);
    requireDone(database_, statement.get());
    return sqlite3_changes(database_) != 0;
}

bool LibraryStore::removeTrackFromLibrary(const TrackId& trackId)
{
    std::lock_guard lock(databaseMutex_);
    const auto track = findTrack(trackId);
    if (!track) {
        return false;
    }
    Transaction transaction(database_);
    Statement statement(database_, "DELETE FROM tracks WHERE id=?");
    bindText(statement.get(), 1, trackId);
    requireDone(database_, statement.get());
    transaction.commit();

    std::error_code error;
    std::filesystem::remove(absoluteTrackPath(*track), error);
    return true;
}

void LibraryStore::applyImportBatch(
    const std::vector<TrackRecord>& tracks,
    const std::vector<std::pair<PlaylistId, TrackId>>& memberships)
{
    std::lock_guard lock(databaseMutex_);
    Transaction transaction(database_);
    for (const TrackRecord& track : tracks) {
        Statement insert(database_, R"SQL(
            INSERT INTO tracks(id, relative_path, title, album, duration_ms, format_label, artwork_relative_path, added_at_ms)
            VALUES(?,?,?,?,?,?,?,?)
            ON CONFLICT(id) DO UPDATE SET
                title=CASE WHEN tracks.title='' OR tracks.title='Unknown Title' THEN excluded.title ELSE tracks.title END,
                album=CASE WHEN tracks.album='' OR tracks.album='Unknown Album' THEN excluded.album ELSE tracks.album END,
                duration_ms=CASE WHEN tracks.duration_ms=0 THEN excluded.duration_ms ELSE tracks.duration_ms END,
                format_label=CASE WHEN excluded.format_label='' THEN tracks.format_label ELSE excluded.format_label END,
                artwork_relative_path=CASE WHEN tracks.artwork_relative_path='' THEN excluded.artwork_relative_path ELSE tracks.artwork_relative_path END
        )SQL");
        bindText(insert.get(), 1, track.id);
        bindText(insert.get(), 2, track.relativePath.string());
        bindText(insert.get(), 3, track.title);
        bindText(insert.get(), 4, track.album);
        sqlite3_bind_int64(insert.get(), 5, track.durationMs);
        bindText(insert.get(), 6, track.formatLabel);
        bindText(insert.get(), 7, track.artworkRelativePath.string());
        sqlite3_bind_int64(insert.get(), 8, track.addedAtMs);
        requireDone(database_, insert.get());

        Statement stats(database_, "INSERT OR IGNORE INTO track_stats(track_id) VALUES(?)");
        bindText(stats.get(), 1, track.id);
        requireDone(database_, stats.get());

        if (!track.artists.empty()) {
            Statement artistCount(database_, "SELECT COUNT(*) FROM track_artists WHERE track_id=?");
            bindText(artistCount.get(), 1, track.id);
            const bool replaceArtists = sqlite3_step(artistCount.get()) == SQLITE_ROW
                && (sqlite3_column_int64(artistCount.get(), 0) == 0
                    || (sqlite3_column_int64(artistCount.get(), 0) == 1
                        && [&] {
                            Statement unknown(database_, "SELECT 1 FROM track_artists WHERE track_id=? AND name='Unknown Artist'");
                            bindText(unknown.get(), 1, track.id);
                            return sqlite3_step(unknown.get()) == SQLITE_ROW;
                        }()));
            if (replaceArtists) {
                Statement clearArtists(database_, "DELETE FROM track_artists WHERE track_id=?");
                bindText(clearArtists.get(), 1, track.id);
                requireDone(database_, clearArtists.get());
                for (std::size_t index = 0; index < track.artists.size(); ++index) {
                    Statement artist(database_, "INSERT INTO track_artists(track_id,position,name) VALUES(?,?,?)");
                    bindText(artist.get(), 1, track.id);
                    sqlite3_bind_int64(artist.get(), 2, static_cast<std::int64_t>(index));
                    bindText(artist.get(), 3, track.artists[index]);
                    requireDone(database_, artist.get());
                }
            }
        }
    }

    for (const auto& [playlistId, trackId] : memberships) {
        addTrackToPlaylist(playlistId, trackId);
    }
    transaction.commit();
}

void LibraryStore::applyStatisticDeltas(const std::vector<StatisticDelta>& deltas)
{
    std::lock_guard lock(databaseMutex_);
    if (deltas.empty()) {
        return;
    }
    Transaction transaction(database_);
    for (const StatisticDelta& delta : deltas) {
        Statement global(database_, R"SQL(
            UPDATE track_stats SET
                listened_ms=listened_ms+?,
                listen_count=listen_count+?,
                skip_count=skip_count+?,
                last_played_at_ms=CASE WHEN ? IS NULL THEN last_played_at_ms ELSE MAX(COALESCE(last_played_at_ms, 0), ?) END
            WHERE track_id=?
        )SQL");
        sqlite3_bind_int64(global.get(), 1, delta.listenedMs);
        sqlite3_bind_int64(global.get(), 2, delta.listenCount);
        sqlite3_bind_int64(global.get(), 3, delta.skipCount);
        if (delta.lastPlayedAtMs) {
            sqlite3_bind_int64(global.get(), 4, *delta.lastPlayedAtMs);
            sqlite3_bind_int64(global.get(), 5, *delta.lastPlayedAtMs);
        } else {
            sqlite3_bind_null(global.get(), 4);
            sqlite3_bind_null(global.get(), 5);
        }
        bindText(global.get(), 6, delta.trackId);
        requireDone(database_, global.get());

        if (delta.playlistId) {
            Statement playlist(database_, R"SQL(
                INSERT INTO playlist_track_stats(playlist_id,track_id,listened_ms,listen_count,skip_count,last_played_at_ms)
                SELECT ?,?,?,?,?,? WHERE EXISTS(
                    SELECT 1 FROM playlist_tracks WHERE playlist_id=? AND track_id=?
                )
                ON CONFLICT(playlist_id,track_id) DO UPDATE SET
                    listened_ms=listened_ms+excluded.listened_ms,
                    listen_count=listen_count+excluded.listen_count,
                    skip_count=skip_count+excluded.skip_count,
                    last_played_at_ms=CASE WHEN excluded.last_played_at_ms IS NULL THEN last_played_at_ms ELSE MAX(COALESCE(last_played_at_ms, 0), excluded.last_played_at_ms) END
            )SQL");
            sqlite3_bind_int64(playlist.get(), 1, *delta.playlistId);
            bindText(playlist.get(), 2, delta.trackId);
            sqlite3_bind_int64(playlist.get(), 3, delta.listenedMs);
            sqlite3_bind_int64(playlist.get(), 4, delta.listenCount);
            sqlite3_bind_int64(playlist.get(), 5, delta.skipCount);
            if (delta.lastPlayedAtMs) sqlite3_bind_int64(playlist.get(), 6, *delta.lastPlayedAtMs); else sqlite3_bind_null(playlist.get(), 6);
            sqlite3_bind_int64(playlist.get(), 7, *delta.playlistId);
            bindText(playlist.get(), 8, delta.trackId);
            requireDone(database_, playlist.get());
        }
    }
    transaction.commit();
}

PlaylistSummary LibraryStore::summary(std::optional<PlaylistId> playlistId) const
{
    std::lock_guard lock(databaseMutex_);
    const std::string sql = playlistId
        ? R"SQL(
            SELECT COALESCE(SUM(t.duration_ms),0), COALESCE(SUM(s.listened_ms),0), MAX(s.last_played_at_ms)
            FROM playlist_tracks p JOIN tracks t ON t.id=p.track_id
            LEFT JOIN playlist_track_stats s ON s.playlist_id=p.playlist_id AND s.track_id=p.track_id
            WHERE p.playlist_id=?
        )SQL"
        : R"SQL(
            SELECT COALESCE(SUM(t.duration_ms),0), COALESCE(SUM(s.listened_ms),0), MAX(s.last_played_at_ms)
            FROM tracks t JOIN track_stats s ON s.track_id=t.id
        )SQL";
    Statement statement(database_, sql);
    if (playlistId) {
        sqlite3_bind_int64(statement.get(), 1, *playlistId);
    }
    PlaylistSummary result;
    if (sqlite3_step(statement.get()) == SQLITE_ROW) {
        result.totalDurationMs = sqlite3_column_int64(statement.get(), 0);
        result.listenedMs = sqlite3_column_int64(statement.get(), 1);
        if (sqlite3_column_type(statement.get(), 2) != SQLITE_NULL) {
            result.lastPlayedAtMs = sqlite3_column_int64(statement.get(), 2);
        }
    }
    return result;
}

} // namespace womp
