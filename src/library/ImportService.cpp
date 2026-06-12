#include "womp/library/ImportService.h"

#include <cairo.h>
#include <gio/gio.h>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/pbutils/pbutils.h>
#include <gst/tag/tag.h>
#include <gst/video/video.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <fstream>
#include <mutex>
#include <ranges>
#include <set>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

namespace womp {

namespace {

void initializeGstreamer()
{
    static std::once_flag flag;
    std::call_once(flag, [] { gst_init(nullptr, nullptr); });
}

std::int64_t unixTimeMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string fallbackTitle(const std::filesystem::path& path)
{
    const auto stem = path.stem().string();
    return stem.empty() ? path.filename().string() : stem;
}

std::string lowercaseAscii(std::string value)
{
    std::ranges::transform(value, value.begin(), [](unsigned char character) {
        return character >= 'A' && character <= 'Z'
            ? static_cast<char>(character - 'A' + 'a')
            : static_cast<char>(character);
    });
    return value;
}

std::string uppercaseFileExtension(const std::filesystem::path& path)
{
    std::string extension = path.extension().string();
    if (!extension.empty() && extension.front() == '.') {
        extension.erase(extension.begin());
    }
    std::ranges::transform(extension, extension.begin(), [](unsigned char character) {
        return character >= 'a' && character <= 'z'
            ? static_cast<char>(character - 'a' + 'A')
            : static_cast<char>(character);
    });
    return extension;
}

std::string checksum(const std::vector<std::uint8_t>& bytes)
{
    GChecksum* value = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(value, bytes.data(), bytes.size());
    std::string result = g_checksum_get_string(value);
    g_checksum_free(value);
    return result;
}

bool readTagString(const GstTagList* tags, const char* name, std::string& output)
{
    gchar* value = nullptr;
    if (tags == nullptr || !gst_tag_list_get_string(tags, name, &value)) {
        return false;
    }
    output = value;
    g_free(value);
    return true;
}

std::vector<std::uint8_t> sampleBytes(GstSample* sample)
{
    if (sample == nullptr) {
        return {};
    }
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstMapInfo map{};
    if (buffer == nullptr || !gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        return {};
    }
    std::vector<std::uint8_t> bytes(map.data, map.data + map.size);
    gst_buffer_unmap(buffer, &map);
    return bytes;
}

struct PngMemoryReader {
    const std::vector<std::uint8_t>& bytes;
    std::size_t offset = 0;
};

cairo_status_t readPngMemory(void* closure, unsigned char* data, unsigned int length)
{
    auto& reader = *static_cast<PngMemoryReader*>(closure);
    if (reader.offset + length > reader.bytes.size()) {
        return CAIRO_STATUS_READ_ERROR;
    }
    std::copy_n(reader.bytes.data() + reader.offset, length, data);
    reader.offset += length;
    return CAIRO_STATUS_SUCCESS;
}

bool normalizePng(const std::vector<std::uint8_t>& bytes, const std::filesystem::path& destination)
{
    if (bytes.size() < 8 || !std::equal(bytes.begin(), bytes.begin() + 8, std::array<std::uint8_t, 8>{137, 80, 78, 71, 13, 10, 26, 10}.begin())) {
        return false;
    }
    PngMemoryReader reader{bytes};
    cairo_surface_t* source = cairo_image_surface_create_from_png_stream(readPngMemory, &reader);
    if (cairo_surface_status(source) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(source);
        return false;
    }
    const int width = cairo_image_surface_get_width(source);
    const int height = cairo_image_surface_get_height(source);
    if (width <= 0 || height <= 0) {
        cairo_surface_destroy(source);
        return false;
    }

    cairo_surface_t* output = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 256, 256);
    cairo_t* cairo = cairo_create(output);
    const double side = static_cast<double>(std::min(width, height));
    const double x = (static_cast<double>(width) - side) * 0.5;
    const double y = (static_cast<double>(height) - side) * 0.5;
    cairo_scale(cairo, 256.0 / side, 256.0 / side);
    cairo_set_source_surface(cairo, source, -x, -y);
    cairo_pattern_set_filter(cairo_get_source(cairo), CAIRO_FILTER_BEST);
    cairo_paint(cairo);
    cairo_destroy(cairo);
    const bool success = cairo_surface_write_to_png(output, destination.c_str()) == CAIRO_STATUS_SUCCESS;
    cairo_surface_destroy(output);
    cairo_surface_destroy(source);
    return success;
}

bool normalizeImageFile(const std::filesystem::path& sourcePath, const std::filesystem::path& destination)
{
    GstElement* pipeline = gst_pipeline_new("artwork-decoder");
    GstElement* source = gst_element_factory_make("filesrc", nullptr);
    GstElement* decoder = gst_element_factory_make("decodebin", nullptr);
    GstElement* converter = gst_element_factory_make("videoconvert", nullptr);
    GstElement* sink = gst_element_factory_make("appsink", nullptr);
    if (pipeline == nullptr || source == nullptr || decoder == nullptr || converter == nullptr || sink == nullptr) {
        if (pipeline != nullptr) gst_object_unref(pipeline);
        return false;
    }
    g_object_set(source, "location", sourcePath.c_str(), nullptr);
    g_object_set(sink, "sync", FALSE, nullptr);
    GstCaps* desiredCaps = gst_caps_from_string("video/x-raw,format=BGRA");
    gst_app_sink_set_caps(GST_APP_SINK(sink), desiredCaps);
    gst_caps_unref(desiredCaps);
    gst_bin_add_many(GST_BIN(pipeline), source, decoder, converter, sink, nullptr);
    if (!gst_element_link(source, decoder) || !gst_element_link(converter, sink)) {
        gst_object_unref(pipeline);
        return false;
    }
    g_signal_connect(decoder, "pad-added", G_CALLBACK(+[] (GstElement*, GstPad* pad, gpointer data) {
        GstElement* converterElement = GST_ELEMENT(data);
        GstPad* sinkPad = gst_element_get_static_pad(converterElement, "sink");
        if (!gst_pad_is_linked(sinkPad)) {
            GstCaps* caps = gst_pad_get_current_caps(pad);
            const GstStructure* structure = caps == nullptr ? nullptr : gst_caps_get_structure(caps, 0);
            const char* name = structure == nullptr ? "" : gst_structure_get_name(structure);
            if (g_str_has_prefix(name, "video/")) {
                gst_pad_link(pad, sinkPad);
            }
            if (caps != nullptr) gst_caps_unref(caps);
        }
        gst_object_unref(sinkPad);
    }), converter);

    bool success = false;
    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE) {
        GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 10 * GST_SECOND);
        if (sample != nullptr) {
            GstCaps* caps = gst_sample_get_caps(sample);
            GstVideoInfo info{};
            GstBuffer* buffer = gst_sample_get_buffer(sample);
            GstMapInfo map{};
            if (caps != nullptr && gst_video_info_from_caps(&info, caps)
                && buffer != nullptr && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
                cairo_surface_t* sourceSurface = cairo_image_surface_create_for_data(
                    map.data,
                    CAIRO_FORMAT_ARGB32,
                    static_cast<int>(GST_VIDEO_INFO_WIDTH(&info)),
                    static_cast<int>(GST_VIDEO_INFO_HEIGHT(&info)),
                    GST_VIDEO_INFO_PLANE_STRIDE(&info, 0));
                cairo_surface_t* output = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 256, 256);
                cairo_t* cairo = cairo_create(output);
                const double width = GST_VIDEO_INFO_WIDTH(&info);
                const double height = GST_VIDEO_INFO_HEIGHT(&info);
                const double side = std::min(width, height);
                cairo_scale(cairo, 256.0 / side, 256.0 / side);
                cairo_set_source_surface(cairo, sourceSurface, -(width - side) * 0.5, -(height - side) * 0.5);
                cairo_pattern_set_filter(cairo_get_source(cairo), CAIRO_FILTER_BEST);
                cairo_paint(cairo);
                cairo_destroy(cairo);
                success = cairo_surface_write_to_png(output, destination.c_str()) == CAIRO_STATUS_SUCCESS;
                cairo_surface_destroy(output);
                cairo_surface_destroy(sourceSurface);
                gst_buffer_unmap(buffer, &map);
            }
            gst_sample_unref(sample);
        }
    }
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    return success;
}

std::vector<std::uint8_t> readBytes(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return file ? std::vector<std::uint8_t>(
                      std::istreambuf_iterator<char>(file),
                      std::istreambuf_iterator<char>())
                : std::vector<std::uint8_t>{};
}

std::optional<std::filesystem::path> siblingArtwork(const std::filesystem::path& audio)
{
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(audio.parent_path(), error)) {
        if (error || !entry.is_regular_file(error)) {
            continue;
        }
        const std::string stem = lowercaseAscii(entry.path().stem().string());
        const std::string extension = lowercaseAscii(entry.path().extension().string());
        if ((stem == "cover" || stem == "folder" || stem == "front")
            && (extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".webp")) {
            return entry.path();
        }
    }
    return std::nullopt;
}

std::string_view trim(std::string_view value)
{
    const std::size_t first = value.find_first_not_of(" \t");
    if (first == std::string_view::npos) return {};
    const std::size_t last = value.find_last_not_of(" \t");
    return value.substr(first, last - first + 1);
}

bool isUrl(std::string_view value)
{
    const std::size_t colon = value.find(':');
    if (colon == std::string_view::npos || colon == 0) return false;
    return std::ranges::all_of(value.substr(0, colon), [](unsigned char character) {
        return std::isalnum(character) != 0 || character == '+' || character == '-' || character == '.';
    });
}

} // namespace

ImportService::ImportService(LibraryStore& store)
    : store_(store)
{
    initializeGstreamer();
}

ImportService::Metadata ImportService::discover(
    const std::filesystem::path& path,
    GstDiscoverer* discoverer) const
{
    Metadata metadata{
        .title = fallbackTitle(path),
        .album = "Unknown Album",
        .artists = {"Unknown Artist"},
        .formatLabel = uppercaseFileExtension(path),
    };
    if (discoverer == nullptr) {
        return metadata;
    }
    GError* error = nullptr;
    gchar* uri = gst_filename_to_uri(path.c_str(), nullptr);
    GstDiscovererInfo* info = uri == nullptr ? nullptr : gst_discoverer_discover_uri(discoverer, uri, &error);
    g_free(uri);
    if (info == nullptr || gst_discoverer_info_get_result(info) != GST_DISCOVERER_OK) {
        if (info != nullptr) gst_discoverer_info_unref(info);
        g_clear_error(&error);
        return metadata;
    }

    GList* audioStreams = gst_discoverer_info_get_audio_streams(info);
    metadata.playable = audioStreams != nullptr;
    gst_discoverer_stream_info_list_free(audioStreams);
    metadata.durationMs = static_cast<std::int64_t>(gst_discoverer_info_get_duration(info) / GST_MSECOND);

    const GstTagList* tags = gst_discoverer_info_get_tags(info);
    readTagString(tags, GST_TAG_TITLE, metadata.title);
    readTagString(tags, GST_TAG_ALBUM, metadata.album);
    metadata.artists.clear();
    if (tags != nullptr) {
        const guint artistCount = gst_tag_list_get_tag_size(tags, GST_TAG_ARTIST);
        for (guint index = 0; index < artistCount; ++index) {
            gchar* artist = nullptr;
            if (gst_tag_list_get_string_index(tags, GST_TAG_ARTIST, index, &artist)) {
                metadata.artists.emplace_back(artist);
                g_free(artist);
            }
        }
        GstSample* image = nullptr;
        const guint imageCount = gst_tag_list_get_tag_size(tags, GST_TAG_IMAGE);
        for (guint index = 0; index < imageCount; ++index) {
            GstSample* candidate = nullptr;
            if (!gst_tag_list_get_sample_index(tags, GST_TAG_IMAGE, index, &candidate)) {
                continue;
            }
            gint imageType = GST_TAG_IMAGE_TYPE_UNDEFINED;
            const GstStructure* info = gst_sample_get_info(candidate);
            if (info != nullptr) {
                gst_structure_get_enum(info, "image-type", GST_TYPE_TAG_IMAGE_TYPE, &imageType);
            }
            if (image == nullptr || imageType == GST_TAG_IMAGE_TYPE_FRONT_COVER) {
                if (image != nullptr) gst_sample_unref(image);
                image = candidate;
                if (imageType == GST_TAG_IMAGE_TYPE_FRONT_COVER) break;
            } else {
                gst_sample_unref(candidate);
            }
        }
        if (image == nullptr) {
            gst_tag_list_get_sample(tags, GST_TAG_PREVIEW_IMAGE, &image);
        }
        if (image != nullptr) {
            metadata.artwork = sampleBytes(image);
            gst_sample_unref(image);
        }
    }
    if (metadata.title.empty()) metadata.title = fallbackTitle(path);
    if (metadata.album.empty()) metadata.album = "Unknown Album";
    if (metadata.artists.empty()) metadata.artists = {"Unknown Artist"};

    gst_discoverer_info_unref(info);
    g_clear_error(&error);
    return metadata;
}

std::optional<TrackId> ImportService::hashFile(const std::filesystem::path& source) const
{
    std::ifstream input(source, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    GChecksum* checksum = g_checksum_new(G_CHECKSUM_SHA256);
    std::array<char, 128 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0) {
            g_checksum_update(checksum, reinterpret_cast<const guchar*>(buffer.data()), static_cast<gsize>(count));
        }
    }
    if (!input.eof()) {
        g_checksum_free(checksum);
        return std::nullopt;
    }
    TrackId id = g_checksum_get_string(checksum);
    g_checksum_free(checksum);
    return id;
}

bool ImportService::ensureTrackFile(const std::filesystem::path& source, const TrackId& id) const
{
    const std::filesystem::path destination = store_.dataDirectory() / "tracks" / id;
    std::error_code error;
    if (std::filesystem::exists(destination, error)) {
        return true;
    }

    gchar* uuid = g_uuid_string_random();
    const std::filesystem::path temporary = store_.dataDirectory() / "tmp" / (std::string(uuid) + ".part");
    g_free(uuid);
    std::filesystem::copy_file(source, temporary, std::filesystem::copy_options::overwrite_existing, error);
    if (error) {
        std::filesystem::remove(temporary);
        return false;
    }
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        error.clear();
        if (std::filesystem::exists(destination, error)) {
            std::filesystem::remove(temporary, error);
            return true;
        }
        std::filesystem::remove(temporary);
        return false;
    }
    return true;
}

std::filesystem::path ImportService::cacheArtwork(
    const TrackId& id,
    const std::filesystem::path& source,
    const std::vector<std::uint8_t>& embedded) const
{
    if (!embedded.empty()) {
        const std::string artworkId = checksum(embedded);
        const std::filesystem::path relative = std::filesystem::path{"artwork"} / (artworkId + ".png");
        const std::filesystem::path destination = store_.dataDirectory() / relative;
        if (std::filesystem::is_regular_file(destination) || normalizePng(embedded, destination)) {
            return relative;
        }
        const std::filesystem::path temporary = store_.dataDirectory() / "tmp" / (id + "-" + artworkId + ".part");
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(embedded.data()), static_cast<std::streamsize>(embedded.size()));
        file.close();
        const bool normalized = file && normalizeImageFile(temporary, destination);
        std::filesystem::remove(temporary);
        if (normalized) {
            return relative;
        }
    }
    if (const auto sibling = siblingArtwork(source)) {
        const std::vector<std::uint8_t> bytes = readBytes(*sibling);
        if (bytes.empty()) {
            return {};
        }
        const std::filesystem::path relative = std::filesystem::path{"artwork"} / (checksum(bytes) + ".png");
        const std::filesystem::path destination = store_.dataDirectory() / relative;
        if (std::filesystem::is_regular_file(destination)
            || normalizePng(bytes, destination)
            || normalizeImageFile(*sibling, destination)) {
            return relative;
        }
    }
    return {};
}

ImportResult ImportService::importFiles(
    std::vector<std::filesystem::path> paths,
    const std::vector<PlaylistId>& playlistIds)
{
    return importPaths(std::move(paths), playlistIds, true);
}

ImportResult ImportService::importPaths(
    std::vector<std::filesystem::path> paths,
    const std::vector<PlaylistId>& playlistIds,
    bool sortPaths)
{
    if (sortPaths) {
        std::ranges::sort(paths, {}, [](const auto& path) { return path.lexically_normal().string(); });
        paths.erase(std::ranges::unique(paths).begin(), paths.end());
    }
    ImportResult result;
    std::vector<TrackRecord> records;
    std::vector<std::pair<PlaylistId, TrackId>> memberships;
    std::set<TrackId> batchIds;
    std::unordered_set<TrackId> existingIds;
    std::unordered_map<std::string, std::filesystem::path> siblingArtworkByDirectory;
    for (TrackId& id : store_.loadTrackIds()) {
        existingIds.insert(std::move(id));
    }
    GstDiscoverer* discoverer = nullptr;
    const std::filesystem::path tracksDirectory =
        std::filesystem::absolute(store_.dataDirectory() / "tracks").lexically_normal();
    const auto addMemberships = [&](const TrackId& id) {
        result.trackIds.push_back(id);
        for (PlaylistId playlistId : playlistIds) {
            memberships.emplace_back(playlistId, id);
        }
    };
    for (const std::filesystem::path& path : paths) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error)) {
            ++result.failed;
            continue;
        }

        std::optional<TrackId> id;
        const std::filesystem::path absolutePath = std::filesystem::absolute(path, error).lexically_normal();
        if (!error
            && absolutePath.parent_path() == tracksDirectory
            && existingIds.contains(absolutePath.filename().string())) {
            id = absolutePath.filename().string();
        } else {
            id = hashFile(path);
        }
        if (!id) {
            ++result.failed;
            continue;
        }

        const bool duplicate = existingIds.contains(*id) || batchIds.contains(*id);
        if (duplicate) {
            if (!ensureTrackFile(path, *id)) {
                ++result.failed;
                continue;
            }
            ++result.duplicates;
            addMemberships(*id);
            continue;
        }

        if (discoverer == nullptr) {
            GError* discovererError = nullptr;
            discoverer = gst_discoverer_new(10 * GST_SECOND, &discovererError);
            g_clear_error(&discovererError);
        }
        Metadata metadata = discover(path, discoverer);
        if (!metadata.playable) {
            ++result.unsupported;
            continue;
        }
        if (!ensureTrackFile(path, *id)) {
            ++result.failed;
            continue;
        }

        batchIds.insert(*id);
        ++result.imported;
        std::filesystem::path artworkRelativePath;
        if (metadata.artwork.empty()) {
            const std::string directory = path.parent_path().lexically_normal().string();
            auto [artwork, inserted] = siblingArtworkByDirectory.try_emplace(directory);
            if (inserted) {
                artwork->second = cacheArtwork(*id, path, metadata.artwork);
            }
            artworkRelativePath = artwork->second;
        } else {
            artworkRelativePath = cacheArtwork(*id, path, metadata.artwork);
        }
        TrackRecord track{
            .id = *id,
            .relativePath = std::filesystem::path{"tracks"} / *id,
            .title = std::move(metadata.title),
            .album = std::move(metadata.album),
            .artists = std::move(metadata.artists),
            .durationMs = metadata.durationMs,
            .formatLabel = std::move(metadata.formatLabel),
            .artworkRelativePath = std::move(artworkRelativePath),
            .addedAtMs = unixTimeMs(),
        };
        records.push_back(std::move(track));
        addMemberships(*id);
    }
    if (discoverer != nullptr) {
        g_object_unref(discoverer);
    }
    store_.applyImportBatch(records, memberships);
    return result;
}

ImportResult ImportService::importM3u(const std::filesystem::path& path, PlaylistId playlistId)
{
    std::ifstream file(path);
    std::vector<std::filesystem::path> entries;
    std::string line;
    bool firstLine = true;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (firstLine && line.starts_with("\xEF\xBB\xBF")) line.erase(0, 3);
        firstLine = false;
        const std::string_view entryText = trim(line);
        if (entryText.empty() || entryText.front() == '#' || isUrl(entryText)) {
            continue;
        }
        std::filesystem::path entry{entryText};
        if (entry.is_relative()) {
            entry = path.parent_path() / entry;
        }
        entries.push_back(std::move(entry));
    }

    return importPaths(std::move(entries), {playlistId}, false);
}

} // namespace womp
