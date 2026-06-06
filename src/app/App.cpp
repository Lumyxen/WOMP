#include "womp/App.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <linux/input-event-codes.h>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>

namespace womp {

struct AudioScanProgress {
    enum class Phase {
        Preparing,
        Indexing,
        Scanning,
        Complete,
    };

    mutable std::mutex mutex;
    Phase phase = Phase::Preparing;
    std::filesystem::path currentPath;
    std::size_t totalRoots = 0;
    std::size_t processedRoots = 0;
    std::size_t discoveredFiles = 0;
    std::size_t totalFiles = 0;
    std::size_t processedFiles = 0;
    std::size_t audioFiles = 0;
    std::uint64_t revision = 0;
};

namespace {

struct AudioScanProgressSnapshot {
    AudioScanProgress::Phase phase = AudioScanProgress::Phase::Preparing;
    std::filesystem::path currentPath;
    std::size_t totalRoots = 0;
    std::size_t processedRoots = 0;
    std::size_t discoveredFiles = 0;
    std::size_t totalFiles = 0;
    std::size_t processedFiles = 0;
    std::size_t audioFiles = 0;
    std::uint64_t revision = 0;
};

template <typename Mutation>
void updateAudioScanProgress(const std::shared_ptr<AudioScanProgress>& progress, Mutation mutate)
{
    if (!progress) {
        return;
    }

    std::scoped_lock lock(progress->mutex);
    mutate(*progress);
    ++progress->revision;
}

AudioScanProgressSnapshot snapshotAudioScanProgress(const std::shared_ptr<AudioScanProgress>& progress)
{
    if (!progress) {
        return {};
    }

    std::scoped_lock lock(progress->mutex);
    return {
        .phase = progress->phase,
        .currentPath = progress->currentPath,
        .totalRoots = progress->totalRoots,
        .processedRoots = progress->processedRoots,
        .discoveredFiles = progress->discoveredFiles,
        .totalFiles = progress->totalFiles,
        .processedFiles = progress->processedFiles,
        .audioFiles = progress->audioFiles,
        .revision = progress->revision,
    };
}

float srgbToLinear(std::uint8_t channel)
{
    const float srgb = static_cast<float>(channel) / 255.0f;
    return srgb <= 0.04045f
        ? srgb / 12.92f
        : std::pow((srgb + 0.055f) / 1.055f, 2.4f);
}

Color rgb(std::uint8_t r, std::uint8_t g, std::uint8_t b, float a = 1.0f)
{
    return {
        .r = srgbToLinear(r),
        .g = srgbToLinear(g),
        .b = srgbToLinear(b),
        .a = a,
    };
}

constexpr float addSongsMenuWidth = 480.0f;
constexpr float addSongsMenuHeight = 236.0f;
constexpr float minAddSongsMenuWidth = 340.0f;
constexpr float minAddSongsMenuHeight = 236.0f;
constexpr float addSongsPlaylistOptionHeight = 36.0f;
constexpr float addSongsMenuButtonGap = 10.0f;
constexpr float addSongsFullListWidth = 360.0f;
constexpr float addSongsFullListMaxHeight = 520.0f;
constexpr float addSongsFullListHeaderHeight = 126.0f;
constexpr float addSongsFullListRowHeight = 30.0f;
constexpr float addSongsFullListBottomPadding = 20.0f;
constexpr float addSongsFullListHorizontalPadding = 20.0f;
constexpr float addSongsFullListScrollbarWidth = 6.0f;
constexpr float addSongsFullListScrollbarHitWidth = 8.0f;
constexpr float addSongsFullListScrollbarInset = 1.0f;
constexpr float floatingMenuMargin = 16.0f;
constexpr char zenityPathSeparator = '\x1f';
constexpr std::chrono::milliseconds addSongsCaretBlinkInterval{500};

struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

Rect centeredFitRect(float windowWidth, float windowHeight, float preferredWidth, float preferredHeight, float minWidth, float minHeight, float margin)
{
    const float usableWidth = std::max(0.0f, windowWidth - margin * 2.0f);
    const float usableHeight = std::max(0.0f, windowHeight - margin * 2.0f);
    const float width = std::clamp(preferredWidth, std::min(minWidth, usableWidth), usableWidth);
    const float height = std::clamp(preferredHeight, std::min(minHeight, usableHeight), usableHeight);
    const float centeredX = (windowWidth - width) * 0.5f;
    const float centeredY = (windowHeight - height) * 0.5f;
    return {
        .x = usableWidth > 0.0f ? std::max(margin, centeredX) : std::max(0.0f, centeredX),
        .y = usableHeight > 0.0f ? std::max(margin, centeredY) : std::max(0.0f, centeredY),
        .width = width,
        .height = height,
    };
}

Rect addSongsMenuRect(
    float windowWidth,
    float windowHeight,
    bool showDirectoryOptions,
    bool playlistDropdownOpen,
    std::size_t playlistCount)
{
    const float playlistOptionsHeight = playlistDropdownOpen
        ? static_cast<float>(std::max<std::size_t>(1, playlistCount)) * addSongsPlaylistOptionHeight
        : 0.0f;
    const float preferredHeight = addSongsMenuHeight
        + (showDirectoryOptions ? 54.0f : 0.0f)
        + playlistOptionsHeight;

    return centeredFitRect(
        windowWidth,
        windowHeight,
        addSongsMenuWidth,
        preferredHeight,
        minAddSongsMenuWidth,
        std::max(minAddSongsMenuHeight, preferredHeight),
        floatingMenuMargin);
}

float addSongsFullListHeight(float windowHeight, std::size_t pendingSongCount)
{
    const float viewportMaxHeight = std::max(0.0f, windowHeight - floatingMenuMargin * 2.0f);
    const float maxHeight = std::min(addSongsFullListMaxHeight, viewportMaxHeight);
    const float fixedHeight = addSongsFullListHeaderHeight + addSongsFullListBottomPadding;
    const float alignedMaxHeight = maxHeight > fixedHeight
        ? fixedHeight + std::floor((maxHeight - fixedHeight) / addSongsFullListRowHeight) * addSongsFullListRowHeight
        : maxHeight;
    const float contentRows = static_cast<float>(std::max<std::size_t>(1, pendingSongCount));
    const float contentHeight = addSongsFullListHeaderHeight
        + contentRows * addSongsFullListRowHeight
        + addSongsFullListBottomPadding;
    return std::min(contentHeight, alignedMaxHeight);
}

Rect addSongsFullListRect(float windowWidth, float windowHeight, const Rect& menu, std::size_t pendingSongCount)
{
    const float gap = 10.0f;
    const float height = addSongsFullListHeight(windowHeight, pendingSongCount);
    const float centeredY = (windowHeight - height) * 0.5f;
    const float y = std::clamp(centeredY, floatingMenuMargin, std::max(floatingMenuMargin, windowHeight - floatingMenuMargin - height));
    const float availableRightWidth = windowWidth - menu.x - menu.width - floatingMenuMargin - gap;
    const float width = std::clamp(addSongsFullListWidth, std::min(280.0f, std::max(0.0f, availableRightWidth)), std::max(0.0f, availableRightWidth));
    if (width > 0.0f) {
        return {
            .x = menu.x + menu.width + gap,
            .y = y,
            .width = width,
            .height = height,
        };
    }

    const float fallbackWidth = std::min(addSongsFullListWidth, std::max(0.0f, menu.x - floatingMenuMargin - gap));
    return {
        .x = std::max(floatingMenuMargin, menu.x - gap - fallbackWidth),
        .y = y,
        .width = fallbackWidth,
        .height = height,
    };
}

Rect pendingSongsListViewportRect(const Rect& panel)
{
    const float availableHeight = std::max(0.0f, panel.height - addSongsFullListHeaderHeight - addSongsFullListBottomPadding);
    return {
        .x = panel.x + addSongsFullListHorizontalPadding,
        .y = panel.y + addSongsFullListHeaderHeight,
        .width = std::max(0.0f, panel.width - addSongsFullListHorizontalPadding * 2.0f),
        .height = std::floor(availableHeight / addSongsFullListRowHeight) * addSongsFullListRowHeight,
    };
}

std::size_t pendingSongsVisibleRowCapacity(const Rect& viewport)
{
    return static_cast<std::size_t>(std::max(0.0f, std::floor(viewport.height / addSongsFullListRowHeight)));
}

float pendingSongsMaxScrollOffset(const Rect& viewport, std::size_t matches)
{
    const std::size_t visibleRows = pendingSongsVisibleRowCapacity(viewport);
    return static_cast<float>(matches > visibleRows ? matches - visibleRows : 0) * addSongsFullListRowHeight;
}

struct PendingSongsScrollbar {
    Rect track;
    Rect thumb;
    float maxScrollOffset = 0.0f;
    float maxThumbTravel = 0.0f;
    bool visible = false;
};

PendingSongsScrollbar pendingSongsScrollbar(const Rect& viewport, std::size_t matches, float scrollOffset)
{
    PendingSongsScrollbar scrollbar{
        .track = {
            .x = viewport.x + viewport.width - addSongsFullListScrollbarInset - addSongsFullListScrollbarWidth,
            .y = viewport.y + addSongsFullListScrollbarInset,
            .width = addSongsFullListScrollbarWidth,
            .height = std::max(0.0f, viewport.height - addSongsFullListScrollbarInset * 2.0f),
        },
    };
    const float totalListHeight = static_cast<float>(matches) * addSongsFullListRowHeight;
    scrollbar.maxScrollOffset = pendingSongsMaxScrollOffset(viewport, matches);
    if (matches == 0 || viewport.height <= 0.0f || totalListHeight <= viewport.height || scrollbar.maxScrollOffset <= 0.0f) {
        return scrollbar;
    }

    scrollbar.visible = true;
    scrollbar.thumb.height = std::min(
        scrollbar.track.height,
        std::max(28.0f, scrollbar.track.height * (viewport.height / totalListHeight)));
    scrollbar.maxThumbTravel = std::max(0.0f, scrollbar.track.height - scrollbar.thumb.height);
    const float scrollRatio = std::clamp(scrollOffset / scrollbar.maxScrollOffset, 0.0f, 1.0f);
    scrollbar.thumb = {
        .x = scrollbar.track.x,
        .y = scrollbar.track.y + scrollbar.maxThumbTravel * scrollRatio,
        .width = addSongsFullListScrollbarWidth,
        .height = scrollbar.thumb.height,
    };
    return scrollbar;
}

bool contains(const Rect& rect, float x, float y)
{
    return x >= rect.x && x <= rect.x + rect.width
        && y >= rect.y && y <= rect.y + rect.height;
}

bool bytesEqual(std::span<const unsigned char> bytes, std::initializer_list<unsigned char> expected)
{
    if (bytes.size() < expected.size()) {
        return false;
    }

    return std::equal(expected.begin(), expected.end(), bytes.begin());
}

bool hasIsoBrand(std::span<const unsigned char> bytes)
{
    if (bytes.size() < 12 || !bytesEqual(bytes.subspan(4), {'f', 't', 'y', 'p'})) {
        return false;
    }

    constexpr std::array<std::array<unsigned char, 4>, 8> audioBrands{{
        {'M', '4', 'A', ' '},
        {'M', '4', 'B', ' '},
        {'m', 'p', '4', '1'},
        {'m', 'p', '4', '2'},
        {'i', 's', 'o', 'm'},
        {'i', 's', 'o', '2'},
        {'d', 'a', 's', 'h'},
        {'3', 'g', 'p', '4'},
    }};

    const auto matchesBrand = [&](std::size_t offset) {
        if (offset + 4 > bytes.size()) {
            return false;
        }

        return std::ranges::any_of(audioBrands, [&](const auto& brand) {
            return std::equal(brand.begin(), brand.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset));
        });
    };

    for (std::size_t offset = 8; offset + 4 <= bytes.size(); offset += 4) {
        if (matchesBrand(offset)) {
            return true;
        }
    }

    return false;
}

bool hasMp3FrameSync(std::span<const unsigned char> bytes)
{
    for (std::size_t index = 0; index + 1 < bytes.size(); ++index) {
        if (bytes[index] == 0xff && (bytes[index + 1] & 0xe0) == 0xe0) {
            return true;
        }
    }

    return false;
}

bool isAudioFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }

    std::array<unsigned char, 64> header{};
    file.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    const std::span bytes(header.data(), static_cast<std::size_t>(std::max<std::streamsize>(0, file.gcount())));

    if (bytes.size() < 4) {
        return false;
    }

    constexpr std::array<unsigned char, 16> asfGuid{
        0x30, 0x26, 0xb2, 0x75, 0x8e, 0x66, 0xcf, 0x11,
        0xa6, 0xd9, 0x00, 0xaa, 0x00, 0x62, 0xce, 0x6c,
    };

    return bytesEqual(bytes, {'I', 'D', '3'})
        || hasMp3FrameSync(bytes)
        || bytesEqual(bytes, {'f', 'L', 'a', 'C'})
        || bytesEqual(bytes, {'O', 'g', 'g', 'S'})
        || (bytes.size() >= 12 && bytesEqual(bytes, {'R', 'I', 'F', 'F'}) && bytesEqual(bytes.subspan(8), {'W', 'A', 'V', 'E'}))
        || (bytes.size() >= 12 && bytesEqual(bytes, {'F', 'O', 'R', 'M'}) && (bytesEqual(bytes.subspan(8), {'A', 'I', 'F', 'F'}) || bytesEqual(bytes.subspan(8), {'A', 'I', 'F', 'C'})))
        || (bytes.size() >= asfGuid.size() && std::equal(asfGuid.begin(), asfGuid.end(), bytes.begin()))
        || hasIsoBrand(bytes);
}

std::filesystem::path homeDirectory()
{
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return home;
    }

    return std::filesystem::current_path();
}

std::filesystem::path dataDirectory()
{
    if (const char* dataHome = std::getenv("XDG_DATA_HOME"); dataHome != nullptr && dataHome[0] != '\0') {
        return std::filesystem::path{dataHome} / "womp";
    }

    return homeDirectory() / ".local" / "share" / "womp";
}

std::filesystem::path configDirectory()
{
    if (const char* configHome = std::getenv("XDG_CONFIG_HOME"); configHome != nullptr && configHome[0] != '\0') {
        return std::filesystem::path{configHome} / "womp";
    }

    return homeDirectory() / ".config" / "womp";
}

std::filesystem::path appConfigFilePath()
{
    return configDirectory() / "config.conf";
}

std::filesystem::path appStateFilePath()
{
    return dataDirectory() / "state.conf";
}

char loadNumberGroupingSeparator()
{
    const std::filesystem::path configPath = appConfigFilePath();
    std::ifstream file(configPath);
    if (!file) {
        std::error_code error;
        std::filesystem::create_directories(configPath.parent_path(), error);
        if (!error) {
            std::ofstream defaultConfig(configPath);
            if (defaultConfig) {
                defaultConfig << "# Values: comma, dot, space, none\n"
                              << "numberGroupingSeparator=comma\n";
            }
        }
        return ',';
    }

    std::string line;
    while (std::getline(file, line)) {
        constexpr std::string_view key = "numberGroupingSeparator=";
        if (!line.starts_with(key)) {
            continue;
        }

        const std::string_view value = std::string_view{line}.substr(key.size());
        if (value == "dot") {
            return '.';
        }
        if (value == "space") {
            return ' ';
        }
        if (value == "none") {
            return '\0';
        }
        return ',';
    }

    return ',';
}

std::string formatGroupedNumber(std::size_t value, char separator)
{
    const std::string digits = std::to_string(value);
    if (separator == '\0' || digits.size() <= 3) {
        return digits;
    }

    std::string formatted;
    formatted.reserve(digits.size() + (digits.size() - 1) / 3);
    for (std::size_t index = 0; index < digits.size(); ++index) {
        if (index > 0 && (digits.size() - index) % 3 == 0) {
            formatted.push_back(separator);
        }
        formatted.push_back(digits[index]);
    }
    return formatted;
}

std::filesystem::path loadLastImportDirectory()
{
    std::ifstream file(appStateFilePath());
    std::string line;
    while (std::getline(file, line)) {
        constexpr std::string_view key = "lastImportDirectory=";
        if (line.starts_with(key)) {
            std::filesystem::path path{line.substr(key.size())};
            std::error_code error;
            if (std::filesystem::is_directory(path, error)) {
                return path;
            }
        }
    }

    return homeDirectory();
}

void saveLastImportDirectory(const std::filesystem::path& directory)
{
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) {
        return;
    }

    const std::filesystem::path dataDir = dataDirectory();
    std::filesystem::create_directories(dataDir, error);
    if (error) {
        return;
    }

    std::ofstream file(dataDir / "state.conf", std::ios::trunc);
    if (file) {
        file << "lastImportDirectory=" << directory.string() << '\n';
    }
}

std::string shellQuote(const std::string& value)
{
    std::string quoted = "'";
    for (char character : value) {
        if (character == '\'') {
            quoted += "'\\''";
        } else {
            quoted += character;
        }
    }
    quoted += "'";
    return quoted;
}

std::string zenityFilenameArgument(const std::filesystem::path& directory)
{
    std::filesystem::path filename = directory;
    filename /= "";
    return " --filename=" + shellQuote(filename.string());
}

VulkanRenderer::PrimitiveUpdate combineUpdates(VulkanRenderer::PrimitiveUpdate current, VulkanRenderer::PrimitiveUpdate next)
{
    return static_cast<VulkanRenderer::PrimitiveUpdate>(
        static_cast<std::uint8_t>(current) | static_cast<std::uint8_t>(next));
}

bool includesUpdate(VulkanRenderer::PrimitiveUpdate update, VulkanRenderer::PrimitiveUpdate flag)
{
    return (static_cast<std::uint8_t>(update) & static_cast<std::uint8_t>(flag)) != 0;
}

std::string cachedSourceMarker(const std::string& source)
{
    return source.empty() ? std::string{} : std::string{"cached"};
}

PrimitiveStyle panelStyle(Color fill, float strokeWidth = 1.0f)
{
    return {
        .fill = fill,
        .stroke = rgb(65, 75, 80),
        .strokeWidth = strokeWidth,
    };
}

bool contains(const ButtonPrimitive& button, float x, float y)
{
    return x >= button.x && x <= button.x + button.width
        && y >= button.y && y <= button.y + button.height;
}

bool contains(const TextFieldPrimitive& textField, float x, float y)
{
    return x >= textField.x && x <= textField.x + textField.width
        && y >= textField.y && y <= textField.y + textField.height;
}

std::string lowercaseAscii(std::string text)
{
    std::ranges::transform(text, text.begin(), [](unsigned char character) {
        if (character >= 'A' && character <= 'Z') {
            return static_cast<char>(character - 'A' + 'a');
        }
        return static_cast<char>(character);
    });
    return text;
}

char characterForKey(std::uint32_t key)
{
    switch (key) {
    case KEY_A: return 'a';
    case KEY_B: return 'b';
    case KEY_C: return 'c';
    case KEY_D: return 'd';
    case KEY_E: return 'e';
    case KEY_F: return 'f';
    case KEY_G: return 'g';
    case KEY_H: return 'h';
    case KEY_I: return 'i';
    case KEY_J: return 'j';
    case KEY_K: return 'k';
    case KEY_L: return 'l';
    case KEY_M: return 'm';
    case KEY_N: return 'n';
    case KEY_O: return 'o';
    case KEY_P: return 'p';
    case KEY_Q: return 'q';
    case KEY_R: return 'r';
    case KEY_S: return 's';
    case KEY_T: return 't';
    case KEY_U: return 'u';
    case KEY_V: return 'v';
    case KEY_W: return 'w';
    case KEY_X: return 'x';
    case KEY_Y: return 'y';
    case KEY_Z: return 'z';
    case KEY_0: return '0';
    case KEY_1: return '1';
    case KEY_2: return '2';
    case KEY_3: return '3';
    case KEY_4: return '4';
    case KEY_5: return '5';
    case KEY_6: return '6';
    case KEY_7: return '7';
    case KEY_8: return '8';
    case KEY_9: return '9';
    case KEY_SPACE: return ' ';
    case KEY_MINUS: return '-';
    case KEY_DOT: return '.';
    case KEY_SLASH: return '/';
    default: break;
    }
    return '\0';
}

std::string formatTimestamp(float seconds)
{
    const int totalSeconds = std::max(0, static_cast<int>(std::round(seconds)));
    const int minutes = totalSeconds / 60;
    const int remainder = totalSeconds % 60;

    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%d:%02d", minutes, remainder);
    return buffer;
}

std::vector<std::filesystem::path> parseZenityPaths(const std::string& output)
{
    std::vector<std::filesystem::path> paths;
    std::size_t pathStart = 0;
    while (pathStart <= output.size()) {
        const std::size_t pathEnd = output.find(zenityPathSeparator, pathStart);
        std::string pathText = output.substr(pathStart, pathEnd == std::string::npos ? std::string::npos : pathEnd - pathStart);
        while (!pathText.empty() && (pathText.back() == '\n' || pathText.back() == '\r')) {
            pathText.pop_back();
        }

        if (!pathText.empty()) {
            std::filesystem::path path{pathText};
            std::error_code error;
            if (std::filesystem::is_regular_file(path, error) || std::filesystem::is_directory(path, error)) {
                paths.push_back(std::move(path));
            }
        }

        if (pathEnd == std::string::npos) {
            break;
        }
        pathStart = pathEnd + 1;
    }

    return paths;
}

std::vector<std::filesystem::path> runFileImportDialog(const std::filesystem::path& initialDirectory)
{
    const std::string command = std::string{"zenity --file-selection --multiple --separator=\"$(printf '\\037')\" "}
        + "--title='Import audio files' "
          "--file-filter='Audio files | *.aac *.aiff *.alac *.flac *.m4a *.mp3 *.ogg *.opus *.wav *.wma' "
          "--file-filter='All files | *' "
        + zenityFilenameArgument(initialDirectory)
        + " 2>/dev/null";
    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
        return {};
    }

    std::string output;
    std::array<char, 4096> buffer{};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }
    pclose(pipe);

    return parseZenityPaths(output);
}

std::vector<std::filesystem::path> audioFilesInDirectory(const std::filesystem::path& directory)
{
    std::vector<std::filesystem::path> audioFiles;
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(
        directory,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    const std::filesystem::recursive_directory_iterator end;

    while (!error && iterator != end) {
        const std::filesystem::directory_entry& entry = *iterator;
        if (entry.is_regular_file(error) && !error && isAudioFile(entry.path())) {
            const std::filesystem::path normalizedPath = std::filesystem::absolute(entry.path(), error).lexically_normal();
            if (!error) {
                audioFiles.push_back(normalizedPath);
            }
        }

        error.clear();
        iterator.increment(error);
    }

    std::ranges::sort(audioFiles);
    return audioFiles;
}

void collectRegularFilesInDirectory(
    const std::filesystem::path& directory,
    std::vector<std::filesystem::path>& files,
    const std::shared_ptr<AudioScanProgress>& progress)
{
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(
        directory,
        std::filesystem::directory_options::skip_permission_denied,
        error);
    const std::filesystem::recursive_directory_iterator end;

    while (!error && iterator != end) {
        const std::filesystem::directory_entry& entry = *iterator;
        std::error_code entryError;
        if (entry.is_regular_file(entryError) && !entryError) {
            const std::filesystem::path normalizedPath = std::filesystem::absolute(entry.path(), entryError).lexically_normal();
            if (!entryError) {
                files.push_back(normalizedPath);
                updateAudioScanProgress(progress, [&](AudioScanProgress& state) {
                    state.phase = AudioScanProgress::Phase::Indexing;
                    state.currentPath = normalizedPath;
                    state.discoveredFiles = files.size();
                });
            }
        }

        error.clear();
        iterator.increment(error);
    }
}

std::vector<std::filesystem::path> expandAudioImportPaths(
    const std::vector<std::filesystem::path>& paths,
    const std::shared_ptr<AudioScanProgress>& progress = {})
{
    std::vector<std::filesystem::path> candidateFiles;
    std::vector<std::filesystem::path> audioFiles;
    updateAudioScanProgress(progress, [&](AudioScanProgress& state) {
        state.phase = AudioScanProgress::Phase::Preparing;
        state.currentPath.clear();
        state.totalRoots = paths.size();
        state.processedRoots = 0;
        state.discoveredFiles = 0;
        state.totalFiles = 0;
        state.processedFiles = 0;
        state.audioFiles = 0;
    });

    std::size_t processedRoots = 0;
    for (const std::filesystem::path& path : paths) {
        std::error_code error;
        const std::filesystem::path normalizedPath = std::filesystem::absolute(path, error).lexically_normal();
        if (error) {
            ++processedRoots;
            updateAudioScanProgress(progress, [&](AudioScanProgress& state) {
                state.phase = AudioScanProgress::Phase::Indexing;
                state.currentPath = path;
                state.processedRoots = processedRoots;
                state.discoveredFiles = candidateFiles.size();
            });
            continue;
        }

        updateAudioScanProgress(progress, [&](AudioScanProgress& state) {
            state.phase = AudioScanProgress::Phase::Indexing;
            state.currentPath = normalizedPath;
            state.processedRoots = processedRoots;
            state.discoveredFiles = candidateFiles.size();
        });

        if (std::filesystem::is_directory(normalizedPath, error) && !error) {
            collectRegularFilesInDirectory(normalizedPath, candidateFiles, progress);
            ++processedRoots;
            updateAudioScanProgress(progress, [&](AudioScanProgress& state) {
                state.phase = AudioScanProgress::Phase::Indexing;
                state.currentPath = normalizedPath;
                state.processedRoots = processedRoots;
                state.discoveredFiles = candidateFiles.size();
            });
            continue;
        }

        error.clear();
        if (std::filesystem::is_regular_file(normalizedPath, error) && !error) {
            candidateFiles.push_back(normalizedPath);
            updateAudioScanProgress(progress, [&](AudioScanProgress& state) {
                state.phase = AudioScanProgress::Phase::Indexing;
                state.currentPath = normalizedPath;
                state.discoveredFiles = candidateFiles.size();
            });
        }

        ++processedRoots;
        updateAudioScanProgress(progress, [&](AudioScanProgress& state) {
            state.phase = AudioScanProgress::Phase::Indexing;
            state.currentPath = normalizedPath;
            state.processedRoots = processedRoots;
            state.discoveredFiles = candidateFiles.size();
        });
    }

    std::ranges::sort(candidateFiles);
    candidateFiles.erase(std::ranges::unique(candidateFiles).begin(), candidateFiles.end());

    updateAudioScanProgress(progress, [&](AudioScanProgress& state) {
        state.phase = AudioScanProgress::Phase::Scanning;
        state.totalFiles = candidateFiles.size();
        state.processedFiles = 0;
        state.audioFiles = 0;
    });

    std::size_t processedFiles = 0;
    for (const std::filesystem::path& candidateFile : candidateFiles) {
        updateAudioScanProgress(progress, [&](AudioScanProgress& state) {
            state.phase = AudioScanProgress::Phase::Scanning;
            state.currentPath = candidateFile;
            state.processedFiles = processedFiles;
            state.totalFiles = candidateFiles.size();
            state.audioFiles = audioFiles.size();
        });

        if (isAudioFile(candidateFile)) {
            audioFiles.push_back(candidateFile);
        }

        ++processedFiles;
        updateAudioScanProgress(progress, [&](AudioScanProgress& state) {
            state.phase = AudioScanProgress::Phase::Scanning;
            state.currentPath = candidateFile;
            state.processedFiles = processedFiles;
            state.totalFiles = candidateFiles.size();
            state.audioFiles = audioFiles.size();
        });
    }

    std::ranges::sort(audioFiles);
    audioFiles.erase(std::ranges::unique(audioFiles).begin(), audioFiles.end());
    updateAudioScanProgress(progress, [&](AudioScanProgress& state) {
        state.phase = AudioScanProgress::Phase::Complete;
        state.processedRoots = state.totalRoots;
        state.processedFiles = state.totalFiles;
        state.audioFiles = audioFiles.size();
    });
    return audioFiles;
}

std::string displayNameForPath(const std::filesystem::path& path)
{
    const std::filesystem::path filename = path.filename();
    if (filename.empty()) {
        return path.string();
    }

    const std::filesystem::path stem = filename.stem();
    return stem.empty() ? filename.string() : stem.string();
}

PendingAudioFile makePendingAudioFile(std::filesystem::path path)
{
    std::string displayName = displayNameForPath(path);
    return {
        .path = std::move(path),
        .displayName = displayName,
        .normalizedDisplayName = lowercaseAscii(std::move(displayName)),
    };
}

bool pendingAudioFilePathLess(const PendingAudioFile& lhs, const PendingAudioFile& rhs)
{
    return lhs.path < rhs.path;
}

bool pendingAudioFilePathEqual(const PendingAudioFile& lhs, const PendingAudioFile& rhs)
{
    return lhs.path == rhs.path;
}

std::string truncateText(std::string text, std::size_t maxCharacters)
{
    if (text.size() <= maxCharacters || maxCharacters <= 3) {
        return text;
    }

    text.resize(maxCharacters - 3);
    text += "...";
    return text;
}

float audioScanProgressFraction(const AudioScanProgressSnapshot& progress)
{
    constexpr float indexingWeight = 0.15f;

    if (progress.phase == AudioScanProgress::Phase::Scanning) {
        if (progress.totalFiles == 0) {
            return 1.0f;
        }

        const float scanFraction = std::clamp(
            static_cast<float>(progress.processedFiles) / static_cast<float>(progress.totalFiles),
            0.0f,
            1.0f);
        return indexingWeight + scanFraction * (1.0f - indexingWeight);
    }

    if (progress.phase == AudioScanProgress::Phase::Complete) {
        return 1.0f;
    }

    if (progress.phase == AudioScanProgress::Phase::Preparing || progress.totalRoots == 0) {
        return 0.0f;
    }

    const float discoveryMomentum = static_cast<float>(progress.discoveredFiles)
        / static_cast<float>(progress.discoveredFiles + 5000);
    const float indexFraction = std::clamp(
        (static_cast<float>(progress.processedRoots) + discoveryMomentum)
            / static_cast<float>(progress.totalRoots),
        0.0f,
        1.0f);
    return indexFraction * indexingWeight;
}

std::string audioScanStatusText(const AudioScanProgressSnapshot& progress)
{
    if (progress.phase == AudioScanProgress::Phase::Scanning) {
        if (progress.totalFiles == 0) {
            return "Checking 0 of 0 files | 100% | 0 audio";
        }

        const int percent = static_cast<int>(std::round(audioScanProgressFraction(progress) * 100.0f));
        return "Checking " + std::to_string(progress.processedFiles)
            + " of " + std::to_string(progress.totalFiles)
            + " files | " + std::to_string(percent)
            + "% | " + std::to_string(progress.audioFiles)
            + " audio";
    }

    if (progress.phase == AudioScanProgress::Phase::Complete) {
        return "Checked " + std::to_string(progress.totalFiles)
            + " files | 100% | " + std::to_string(progress.audioFiles)
            + " audio";
    }

    if (progress.phase == AudioScanProgress::Phase::Indexing) {
        const int percent = static_cast<int>(std::round(audioScanProgressFraction(progress) * 100.0f));
        return "Indexing "
            + std::to_string(progress.processedRoots)
            + " of " + std::to_string(progress.totalRoots)
            + " locations | " + std::to_string(percent)
            + "% | " + std::to_string(progress.discoveredFiles)
            + " files found";
    }

    return "Preparing scan | 0%";
}

std::string audioScanPathText(const AudioScanProgressSnapshot& progress, std::size_t maxCharacters)
{
    if (progress.currentPath.empty()) {
        return "Waiting for selected files...";
    }

    return truncateText(progress.currentPath.string(), maxCharacters);
}

const std::string& mutedVolumeIcon()
{
    static const std::string icon = R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="lucide lucide-volume-off-icon lucide-volume-off"><path d="M16 9a5 5 0 0 1 .95 2.293"/><path d="M19.364 5.636a9 9 0 0 1 1.889 9.96"/><path d="m2 2 20 20"/><path d="m7 7-.587.587A1.4 1.4 0 0 1 5.416 8H3a1 1 0 0 0-1 1v6a1 1 0 0 0 1 1h2.416a1.4 1.4 0 0 1 .997.413l3.383 3.384A.705.705 0 0 0 11 19.298V11"/><path d="M9.828 4.172A.686.686 0 0 1 11 4.657v.686"/></svg>)";
    return icon;
}

const std::string& volumeZeroIcon()
{
    static const std::string icon = R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="lucide lucide-volume-icon lucide-volume"><path d="M11 4.702a.705.705 0 0 0-1.203-.498L6.413 7.587A1.4 1.4 0 0 1 5.416 8H3a1 1 0 0 0-1 1v6a1 1 0 0 0 1 1h2.416a1.4 1.4 0 0 1 .997.413l3.383 3.384A.705.705 0 0 0 11 19.298z"/></svg>)";
    return icon;
}

const std::string& volumeOneIcon()
{
    static const std::string icon = R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="lucide lucide-volume1-icon lucide-volume-1"><path d="M11 4.702a.705.705 0 0 0-1.203-.498L6.413 7.587A1.4 1.4 0 0 1 5.416 8H3a1 1 0 0 0-1 1v6a1 1 0 0 0 1 1h2.416a1.4 1.4 0 0 1 .997.413l3.383 3.384A.705.705 0 0 0 11 19.298z"/><path d="M16 9a5 5 0 0 1 0 6"/></svg>)";
    return icon;
}

const std::string& volumeTwoIcon()
{
    static const std::string icon = R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="lucide lucide-volume2-icon lucide-volume-2"><path d="M11 4.702a.705.705 0 0 0-1.203-.498L6.413 7.587A1.4 1.4 0 0 1 5.416 8H3a1 1 0 0 0-1 1v6a1 1 0 0 0 1 1h2.416a1.4 1.4 0 0 1 .997.413l3.383 3.384A.705.705 0 0 0 11 19.298z"/><path d="M16 9a5 5 0 0 1 0 6"/><path d="M19.364 18.364a9 9 0 0 0 0-12.728"/></svg>)";
    return icon;
}

const std::string& volumeIconFor(float effectiveVolume, bool muted)
{
    if (muted || effectiveVolume <= 0.0f) {
        return mutedVolumeIcon();
    }
    if (effectiveVolume <= 0.25f) {
        return volumeZeroIcon();
    }
    if (effectiveVolume <= 0.5f) {
        return volumeOneIcon();
    }
    return volumeTwoIcon();
}

} // namespace

void App::buildInitialScene(float windowWidth, float windowHeight)
{
    constexpr float preferredSidebarWidth = 280.0f;
    constexpr float minSidebarWidth = 180.0f;
    constexpr float sidebarPadding = 16.0f;
    constexpr float sidebarButtonHeight = 36.0f;
    constexpr float sidebarButtonGap = 4.0f;
    const float sidebarWidth = std::clamp(windowWidth * 0.32f, minSidebarWidth, preferredSidebarWidth);
    const float sidebarButtonWidth = std::max(0.0f, sidebarWidth - sidebarPadding * 2.0f);
    constexpr float bottomBarHeight = 72.0f;
    constexpr float transportButtonSize = 40.0f;
    constexpr float shuffleButtonSize = 34.0f;
    constexpr float volumeButtonSize = 40.0f;
    constexpr float transportButtonGap = 8.0f;
    constexpr float volumeControlGap = 8.0f;
    constexpr float transportIconSize = 22.0f;
    constexpr float shuffleIconSize = 18.0f;
    constexpr float volumeIconSize = 22.0f;
    constexpr float maxVolumeSliderWidth = 112.0f;
    constexpr float minVolumeSliderWidth = 56.0f;
    constexpr float volumeSliderHeight = 4.0f;
    constexpr float volumeSliderKnobRadius = 6.0f;
    constexpr float maxTransportBarWidth = 640.0f;
    constexpr float minTransportBarWidth = 120.0f;
    constexpr float transportBarThickness = 4.0f;
    constexpr float transportBarKnobRadius = 6.0f;
    constexpr float timestampGap = 10.0f;
    constexpr float timestampWidth = 46.0f;
    constexpr float timestampFontSize = 13.0f;
    constexpr float playlistButtonHeight = 56.0f;
    constexpr float playlistButtonGap = 8.0f;
    const Color sidebarBackground = rgb(45, 53, 59);
    const Color transparent = {0.0f, 0.0f, 0.0f, 0.0f};
    const Color sidebarText = rgb(211, 198, 170);
    const Color iconGrey = rgb(133, 146, 137);
    const Color accent = rgb(167, 192, 128);
    const Color border = rgb(71, 82, 88);
    const Color mantle = rgb(52, 63, 68);
    const Color activeFill = rgb(63, 74, 69);
    const Color separator = rgb(71, 82, 88);
    const Color transportBar = rgb(133, 146, 137);
    const Color timestampText = rgb(211, 198, 170);

    const std::string addSongsIcon = R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#859289" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="lucide lucide-plus-icon lucide-plus"><path d="M5 12h14"/><path d="M12 5v14"/></svg>)";
    const std::string chevronUpIcon = R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#ffffff" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="lucide lucide-chevron-up-icon lucide-chevron-up"><path d="m18 15-6-6-6 6"/></svg>)";
    const std::string chevronDownIcon = R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#ffffff" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="lucide lucide-chevron-down-icon lucide-chevron-down"><path d="m6 9 6 6 6-6"/></svg>)";
    const std::string checkIcon = R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="lucide lucide-check-icon lucide-check"><path d="M20 6 9 17l-5-5"/></svg>)";
    const std::string closeIcon = R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="lucide lucide-x-icon lucide-x"><path d="M18 6 6 18"/><path d="m6 6 12 12"/></svg>)";
    const std::string settingsIcon = R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#859289" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="lucide lucide-settings2-icon lucide-settings-2"><path d="M14 17H5"/><path d="M19 7h-9"/><circle cx="17" cy="17" r="3"/><circle cx="7" cy="7" r="3"/></svg>)";
    const std::string playlistIcon = R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#859289" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="lucide lucide-list-music-icon lucide-list-music"><path d="M16 5H3"/><path d="M11 12H3"/><path d="M11 19H3"/><path d="M21 16V5"/><circle cx="18" cy="16" r="3"/></svg>)";
    const std::string pinIcon = R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="lucide lucide-pin-icon lucide-pin"><path d="M12 17v5"/><path d="M9 10.76a2 2 0 0 1-1.11 1.79l-1.78.9A2 2 0 0 0 5 15.24V16a1 1 0 0 0 1 1h12a1 1 0 0 0 1-1v-.76a2 2 0 0 0-1.11-1.79l-1.78-.9A2 2 0 0 1 15 10.76V7a1 1 0 0 1 1-1 2 2 0 0 0 0-4H8a2 2 0 0 0 0 4 1 1 0 0 1 1 1z"/></svg>)";
    const std::string renameIcon = R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#ffffff" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="lucide lucide-square-pen-icon lucide-square-pen"><path d="M12 3H5a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2v-7"/><path d="M18.375 2.625a1 1 0 0 1 3 3l-9.013 9.014a2 2 0 0 1-.853.505l-2.873.84a.5.5 0 0 1-.62-.62l.84-2.873a2 2 0 0 1 .506-.852z"/></svg>)";
    const std::string exportIcon = R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#ffffff" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="lucide lucide-upload-icon lucide-upload"><path d="M12 3v12"/><path d="m17 8-5-5-5 5"/><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/></svg>)";
    const std::string shuffleIcon = R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#ffffff" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="lucide lucide-shuffle-icon lucide-shuffle"><path d="m18 14 4 4-4 4"/><path d="m18 2 4 4-4 4"/><path d="M2 18h1.973a4 4 0 0 0 3.3-1.7l5.454-8.6a4 4 0 0 1 3.3-1.7H22"/><path d="M2 6h1.972a4 4 0 0 1 3.6 2.2"/><path d="M22 18h-6.041a4 4 0 0 1-3.3-1.8l-.359-.45"/></svg>)";
    const std::string backwardIcon = R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="lucide lucide-skip-back-icon lucide-skip-back"><path d="M17.971 4.285A2 2 0 0 1 21 6v12a2 2 0 0 1-3.029 1.715l-9.997-5.998a2 2 0 0 1-.003-3.432z"/><path d="M3 20V4"/></svg>)";
    const std::string pauseIcon = R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="lucide lucide-pause-icon lucide-pause"><rect x="14" y="3" width="5" height="18" rx="1"/><rect x="5" y="3" width="5" height="18" rx="1"/></svg>)";
    const std::string playIcon = R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#ffffff" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round" class="lucide lucide-play-icon lucide-play"><path d="M5 5a2 2 0 0 1 3.008-1.728l11.997 6.998a2 2 0 0 1 .003 3.458l-12 7A2 2 0 0 1 5 19z"/></svg>)";
    const std::string forwardIcon = R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="lucide lucide-skip-forward-icon lucide-skip-forward"><path d="M21 4v16"/><path d="M6.029 4.285A2 2 0 0 0 3 6v12a2 2 0 0 0 3.029 1.715l9.997-5.998a2 2 0 0 0 .003-3.432z"/></svg>)";
    const bool effectivelyMuted = volumeMuted_ || volume_ <= 0.0f;
    const float effectiveVolume = effectivelyMuted ? 0.0f : volume_;
    const std::string& volumeIcon = volumeIconFor(effectiveVolume, effectivelyMuted);

    const auto addSidebarButton = [&](std::string label, std::string iconSvg, float y, std::function<void()> onClick = {}) {
        primitives_.add(Primitive::button(
            {
                .x = sidebarPadding,
                .y = y,
                .width = sidebarButtonWidth,
                .height = sidebarButtonHeight,
                .padding = 12.0f,
                .radius = 0.0f,
                .fontSize = 15.0f,
                .iconSize = 18.0f,
                .iconGap = 10.0f,
                .label = std::move(label),
                .iconSvg = std::move(iconSvg),
                .iconColor = iconGrey,
                .labelColor = sidebarText,
                .hoverLabelColor = accent,
                .pressedLabelColor = sidebarText,
                .hoverFill = mantle,
                .pressedFill = activeFill,
                .hoverStroke = border,
                .pressedStroke = accent,
                .onClick = std::move(onClick),
                .centerLabel = false,
            },
            {
                .fill = transparent,
                .stroke = transparent,
                .strokeWidth = 1.0f,
            }));
    };

    const auto addPlaylistButton = [&](PlaylistId id, std::string label, std::string iconSvg, float y) {
        const bool selected = selectedPlaylistId_ == id;
        primitives_.add(Primitive::button(
            {
                .x = sidebarPadding,
                .y = y,
                .width = sidebarButtonWidth,
                .height = playlistButtonHeight,
                .padding = 14.0f,
                .radius = 0.0f,
                .fontSize = 17.0f,
                .iconSize = 22.0f,
                .iconGap = 12.0f,
                .label = std::move(label),
                .iconSvg = std::move(iconSvg),
                .iconColor = iconGrey,
                .labelColor = sidebarText,
                .hoverLabelColor = accent,
                .pressedLabelColor = sidebarText,
                .hoverFill = mantle,
                .pressedFill = activeFill,
                .hoverStroke = border,
                .pressedStroke = accent,
                .onClick = [this, id]() {
                    selectPlaylist(id);
                },
                .centerLabel = false,
            },
            {
                .fill = selected ? activeFill : transparent,
                .stroke = selected ? accent : transparent,
                .strokeWidth = 1.0f,
            }));
    };

    const auto addTransportButton = [&](std::string iconSvg, float x, float y, std::function<void()> onClick = {}) {
        return primitives_.add(Primitive::button(
            {
                .x = x,
                .y = y,
                .width = transportButtonSize,
                .height = transportButtonSize,
                .padding = (transportButtonSize - transportIconSize) * 0.5f,
                .radius = 0.0f,
                .iconSize = transportIconSize,
                .label = "",
                .iconSvg = std::move(iconSvg),
                .iconColor = sidebarText,
                .hoverFill = mantle,
                .pressedFill = activeFill,
                .hoverStroke = border,
                .pressedStroke = accent,
                .onClick = std::move(onClick),
            },
            {
                .fill = transparent,
                .stroke = transparent,
                .strokeWidth = 1.0f,
            }));
    };

    const auto addShuffleButton = [&](float x, float y) {
        shuffleButtonId_ = primitives_.add(Primitive::button(
            {
                .x = x,
                .y = y,
                .width = shuffleButtonSize,
                .height = shuffleButtonSize,
                .padding = (shuffleButtonSize - shuffleIconSize) * 0.5f,
                .radius = 0.0f,
                .iconSize = shuffleIconSize,
                .label = "",
                .iconSvg = shuffleIcon,
                .iconColor = shuffleEnabled_ ? accent : iconGrey,
                .hoverFill = mantle,
                .pressedFill = activeFill,
                .hoverStroke = border,
                .pressedStroke = accent,
                .onClick = [this]() {
                    toggleShuffle();
                },
            },
            {
                .fill = transparent,
                .stroke = transparent,
                .strokeWidth = 1.0f,
            }));
    };

    const auto addVolumeButton = [&](float x, float y) {
        volumeButtonId_ = primitives_.add(Primitive::button(
            {
                .x = x,
                .y = y,
                .width = volumeButtonSize,
                .height = volumeButtonSize,
                .padding = (volumeButtonSize - volumeIconSize) * 0.5f,
                .radius = 0.0f,
                .iconSize = volumeIconSize,
                .label = "",
                .iconSvg = volumeIcon,
                .iconColor = effectivelyMuted ? iconGrey : sidebarText,
                .hoverFill = mantle,
                .pressedFill = activeFill,
                .hoverStroke = border,
                .pressedStroke = accent,
                .onClick = [this]() {
                    toggleVolumeMute();
                },
            },
            {
                .fill = transparent,
                .stroke = transparent,
                .strokeWidth = 1.0f,
            }));
    };

    const auto addHeaderActionButton = [&](const std::string& iconSvg, float x, float y, float size, bool primary = false) {
        primitives_.add(Primitive::button(
            {
                .x = x,
                .y = y,
                .width = size,
                .height = size,
                .padding = (size - 20.0f) * 0.5f,
                .radius = 0.0f,
                .iconSize = 20.0f,
                .label = "",
                .iconSvg = iconSvg,
                .iconColor = primary ? sidebarBackground : sidebarText,
                .hoverFill = primary ? sidebarText : mantle,
                .pressedFill = primary ? iconGrey : activeFill,
                .hoverStroke = primary ? sidebarText : border,
                .pressedStroke = accent,
            },
            {
                .fill = primary ? accent : transparent,
                .stroke = primary ? accent : border,
                .strokeWidth = 1.0f,
            }));
    };

    primitives_.add(Primitive::roundedRect(
        {
            .x = 0.0f,
            .y = 0.0f,
            .width = sidebarWidth,
            .height = windowHeight,
            .radius = 0.0f,
        },
        panelStyle(sidebarBackground, 0.0f)));
    primitives_.add(Primitive::roundedRect(
        {
            .x = sidebarWidth,
            .y = windowHeight - bottomBarHeight,
            .width = std::max(0.0f, windowWidth - sidebarWidth),
            .height = bottomBarHeight,
            .radius = 0.0f,
        },
        panelStyle(sidebarBackground, 0.0f)));
    primitives_.add(Primitive::line(
        {
            .x0 = sidebarWidth,
            .y0 = 0.0f,
            .x1 = sidebarWidth,
            .y1 = windowHeight,
            .thickness = 1.0f,
        },
        {
            .fill = separator,
            .stroke = separator,
            .strokeWidth = 0.0f,
        }));
    primitives_.add(Primitive::line(
        {
            .x0 = sidebarWidth,
            .y0 = windowHeight - bottomBarHeight,
            .x1 = windowWidth,
            .y1 = windowHeight - bottomBarHeight,
            .thickness = 1.0f,
        },
        {
            .fill = separator,
            .stroke = separator,
            .strokeWidth = 0.0f,
        }));

    const float mainContentWidth = std::max(0.0f, windowWidth - sidebarWidth);
    const float transportGroupWidth = transportButtonSize * 3.0f + transportButtonGap * 2.0f;
    const float minimumMainX = sidebarWidth + sidebarPadding;
    const float maximumTransportX = std::max(minimumMainX, windowWidth - sidebarPadding - transportGroupWidth);
    const float transportX = std::clamp((windowWidth - transportGroupWidth) * 0.5f, minimumMainX, maximumTransportX);
    const float transportY = windowHeight - bottomBarHeight + 8.0f;
    const float maxResponsiveTransportBarWidth = std::max(
        minTransportBarWidth,
        mainContentWidth - sidebarPadding * 2.0f - (timestampGap + timestampWidth) * 2.0f);
    const float transportBarWidth = std::clamp(maxTransportBarWidth, minTransportBarWidth, maxResponsiveTransportBarWidth);
    const float minimumTransportBarX = sidebarWidth + sidebarPadding + timestampWidth + timestampGap;
    const float maximumTransportBarX = std::max(minimumTransportBarX, windowWidth - sidebarPadding - timestampWidth - timestampGap - transportBarWidth);
    const float transportBarX = std::clamp((windowWidth - transportBarWidth) * 0.5f, minimumTransportBarX, maximumTransportBarX);
    const float transportBarY = transportY + transportButtonSize + 6.0f;
    const float mediaProgress = hasCurrentSong_ && currentSongDurationSeconds_ > 0.0f
        ? std::clamp(currentSongElapsedSeconds_ / currentSongDurationSeconds_, 0.0f, 1.0f)
        : 0.0f;
    const float volumeY = windowHeight - bottomBarHeight + (bottomBarHeight - volumeButtonSize) * 0.5f;
    const float volumeSliderWidth = std::clamp(mainContentWidth * 0.14f, minVolumeSliderWidth, maxVolumeSliderWidth);
    const float volumeControlWidth = volumeButtonSize + volumeControlGap + volumeSliderWidth;
    const float volumeX = std::max(minimumMainX, windowWidth - sidebarPadding - volumeControlWidth);
    volumeSliderX_ = volumeX + volumeButtonSize + volumeControlGap;
    volumeSliderY_ = volumeY + (volumeButtonSize - volumeSliderHeight) * 0.5f;
    volumeSliderWidth_ = volumeSliderWidth;
    volumeSliderHeight_ = volumeSliderHeight;
    volumeSliderHitHeight_ = volumeButtonSize;
    mediaProgressSliderX_ = transportBarX;
    mediaProgressSliderY_ = transportBarY;
    mediaProgressSliderWidth_ = transportBarWidth;
    mediaProgressSliderHeight_ = transportBarThickness;
    mediaProgressSliderHitHeight_ = volumeButtonSize;
    addShuffleButton(transportX - shuffleButtonSize - transportButtonGap, transportY + (transportButtonSize - shuffleButtonSize) * 0.5f);
    addTransportButton(backwardIcon, transportX, transportY);
    playPauseButtonId_ = addTransportButton(playing_ ? pauseIcon : playIcon, transportX + transportButtonSize + transportButtonGap, transportY, [this]() {
        togglePlayback();
    });
    addTransportButton(forwardIcon, transportX + (transportButtonSize + transportButtonGap) * 2.0f, transportY);
    addVolumeButton(volumeX, volumeY);
    primitives_.add(Primitive::roundedRect(
        {
            .x = volumeSliderX_,
            .y = volumeSliderY_,
            .width = volumeSliderWidth_,
            .height = volumeSliderHeight,
            .radius = 0.0f,
        },
        {
            .fill = border,
            .stroke = border,
            .strokeWidth = 0.0f,
        }));
    volumeSliderFillId_ = primitives_.add(Primitive::roundedRect(
        {
            .x = volumeSliderX_,
            .y = volumeSliderY_,
            .width = volumeSliderWidth_ * effectiveVolume,
            .height = volumeSliderHeight,
            .radius = 0.0f,
        },
        {
            .fill = effectivelyMuted ? iconGrey : accent,
            .stroke = effectivelyMuted ? iconGrey : accent,
            .strokeWidth = 0.0f,
        }));
    volumeSliderKnobId_ = primitives_.add(Primitive::circle(
        {
            .centerX = volumeSliderX_ + volumeSliderWidth_ * effectiveVolume,
            .centerY = volumeSliderY_ + volumeSliderHeight * 0.5f,
            .radius = volumeSliderKnobRadius,
        },
        {
            .fill = effectivelyMuted ? iconGrey : sidebarText,
            .stroke = sidebarBackground,
            .strokeWidth = 1.0f,
        }));
    elapsedTimeTextId_ = primitives_.add(Primitive::text(
        {
            .x = mediaProgressSliderX_ - timestampGap - timestampWidth,
            .y = mediaProgressSliderY_ - 7.0f,
            .fontSize = timestampFontSize,
            .text = hasCurrentSong_ ? formatTimestamp(currentSongElapsedSeconds_) : "0:00",
        },
        {
            .fill = timestampText,
            .stroke = timestampText,
            .strokeWidth = 0.0f,
        }));
    primitives_.add(Primitive::roundedRect(
        {
            .x = mediaProgressSliderX_,
            .y = mediaProgressSliderY_,
            .width = mediaProgressSliderWidth_,
            .height = mediaProgressSliderHeight_,
            .radius = 0.0f,
        },
        {
            .fill = transportBar,
            .stroke = transportBar,
            .strokeWidth = 0.0f,
        }));
    mediaProgressSliderFillId_ = primitives_.add(Primitive::roundedRect(
        {
            .x = mediaProgressSliderX_,
            .y = mediaProgressSliderY_,
            .width = mediaProgressSliderWidth_ * mediaProgress,
            .height = mediaProgressSliderHeight_,
            .radius = 0.0f,
        },
        {
            .fill = accent,
            .stroke = accent,
            .strokeWidth = 0.0f,
        }));
    mediaProgressSliderKnobId_ = primitives_.add(Primitive::circle(
        {
            .centerX = mediaProgressSliderX_ + mediaProgressSliderWidth_ * mediaProgress,
            .centerY = mediaProgressSliderY_ + mediaProgressSliderHeight_ * 0.5f,
            .radius = transportBarKnobRadius,
        },
        {
            .fill = canSeekMediaProgress() ? sidebarText : iconGrey,
            .stroke = sidebarBackground,
            .strokeWidth = 1.0f,
        }));
    totalTimeTextId_ = primitives_.add(Primitive::text(
        {
            .x = mediaProgressSliderX_ + mediaProgressSliderWidth_ + timestampGap,
            .y = mediaProgressSliderY_ - 7.0f,
            .fontSize = timestampFontSize,
            .text = hasCurrentSong_ ? formatTimestamp(currentSongDurationSeconds_) : "0:00",
        },
        {
            .fill = timestampText,
            .stroke = timestampText,
            .strokeWidth = 0.0f,
        }));

    const float contentPadding = 28.0f;
    const float contentX = sidebarWidth + contentPadding;
    const float contentY = contentPadding;
    const float contentWidth = std::max(0.0f, windowWidth - sidebarWidth - contentPadding * 2.0f);
    const float contentBottom = windowHeight - bottomBarHeight - contentPadding;
    const float rowHeight = 34.0f;
    const std::size_t maxTitleCharacters = static_cast<std::size_t>(std::max(18.0f, contentWidth / 9.0f));
    const Playlist* selectedPlaylist = nullptr;
    std::string selectedPlaylistName = "All Songs";
    if (selectedPlaylistId_ != 0) {
        if (const auto playlist = std::ranges::find(playlists_, selectedPlaylistId_, &Playlist::id); playlist != playlists_.end()) {
            selectedPlaylist = &*playlist;
            selectedPlaylistName = playlist->name;
        }
    }

    constexpr float headerPadding = 18.0f;
    constexpr float headerGap = 16.0f;
    constexpr float headerActionGap = 8.0f;
    constexpr float preferredHeaderActionSize = 38.0f;
    constexpr float minimumHeaderActionSize = 20.0f;
    constexpr float headerIconTileSize = 116.0f;
    constexpr float headerStatsTop = headerPadding + headerIconTileSize + 12.0f;
    constexpr float headerStatHeight = 48.0f;
    constexpr float headerStatGap = 8.0f;
    const bool allSongsSelected = selectedPlaylist == nullptr;
    const std::size_t playlistSongCount = allSongsSelected ? tracks_.size() : selectedPlaylist->trackIndexes.size();
    const std::string groupedSongCount = formatGroupedNumber(playlistSongCount, numberGroupingSeparator_);
    const std::string playlistDescription = allSongsSelected
        ? "Every song in your library."
        : "No description for this playlist.";
    const std::array<std::pair<std::string_view, std::string>, 4> playlistStats{{
        {"SONGS", groupedSongCount},
        {"TOTAL TIME", "0 min"},
        {"LISTENED", "0 min"},
        {"LAST PLAYED", "Never"},
    }};
    const std::size_t headerActionCount = allSongsSelected ? 2 : 5;
    const float responsiveIconTileSize = std::min(
        headerIconTileSize,
        std::max(72.0f, contentWidth * 0.2f));
    const float headerIconX = contentX + headerPadding;
    const float headerIconY = contentY + headerPadding;
    const float headerTextX = headerIconX + responsiveIconTileSize + headerGap;
    const float headerTextWidth = std::max(0.0f, contentX + contentWidth - headerPadding - headerTextX);
    const std::size_t maxHeaderTitleCharacters = static_cast<std::size_t>(std::max(8.0f, headerTextWidth / 13.0f));
    const std::size_t maxHeaderDescriptionCharacters = static_cast<std::size_t>(std::max(12.0f, headerTextWidth / 7.5f));
    const float headerStatsWidth = std::max(0.0f, contentWidth - headerPadding * 2.0f);
    const std::size_t headerStatColumnCount = headerStatsWidth >= 360.0f ? 4 : 2;
    const std::size_t headerStatRowCount = (playlistStats.size() + headerStatColumnCount - 1) / headerStatColumnCount;
    const float headerStatWidth = std::max(
        0.0f,
        (headerStatsWidth - headerStatGap * static_cast<float>(headerStatColumnCount - 1))
            / static_cast<float>(headerStatColumnCount));
    const float headerStatsHeight = headerStatHeight * static_cast<float>(headerStatRowCount)
        + headerStatGap * static_cast<float>(headerStatRowCount - 1);
    const float headerHeight = headerStatsTop + headerStatsHeight + headerPadding;
    const float availableActionWidth = std::max(
        0.0f,
        headerTextWidth - headerActionGap * static_cast<float>(headerActionCount - 1));
    const float headerActionSize = std::clamp(
        availableActionWidth / static_cast<float>(headerActionCount),
        minimumHeaderActionSize,
        preferredHeaderActionSize);
    const float headerActionsWidth = headerActionSize * static_cast<float>(headerActionCount)
        + headerActionGap * static_cast<float>(headerActionCount - 1);
    const float headerActionsX = std::max(
        headerTextX,
        contentX + contentWidth - headerPadding - headerActionsWidth);
    const float headerActionsY = contentY + headerPadding + headerIconTileSize - headerActionSize;

    primitives_.add(Primitive::roundedRect(
        {
            .x = contentX,
            .y = contentY,
            .width = contentWidth,
            .height = headerHeight,
            .radius = 0.0f,
        },
        panelStyle(sidebarBackground)));
    primitives_.add(Primitive::roundedRect(
        {
            .x = headerIconX,
            .y = headerIconY,
            .width = responsiveIconTileSize,
            .height = responsiveIconTileSize,
            .radius = 0.0f,
        },
        {
            .fill = mantle,
            .stroke = border,
            .strokeWidth = 1.0f,
        }));
    primitives_.add(Primitive::svg(
        {
            .x = headerIconX,
            .y = headerIconY,
            .width = responsiveIconTileSize,
            .height = responsiveIconTileSize,
            .rasterScale = 4.0f,
            .source = playlistIcon,
            .sourceType = SvgSourceType::Data,
            .renderMode = SvgRenderMode::Mask,
        },
        {
            .fill = accent,
            .stroke = transparent,
            .strokeWidth = 0.0f,
        }));
    primitives_.add(Primitive::text(
        {
            .x = headerTextX,
            .y = contentY + 22.0f,
            .fontSize = 11.0f,
            .text = "PLAYLIST",
        },
        {
            .fill = iconGrey,
            .stroke = iconGrey,
            .strokeWidth = 0.0f,
        }));
    primitives_.add(Primitive::text(
        {
            .x = headerTextX,
            .y = contentY + 42.0f,
            .fontSize = 25.0f,
            .text = truncateText(selectedPlaylistName, maxHeaderTitleCharacters),
        },
        {
            .fill = sidebarText,
            .stroke = sidebarText,
            .strokeWidth = 0.0f,
        }));
    primitives_.add(Primitive::text(
        {
            .x = headerTextX,
            .y = contentY + 78.0f,
            .fontSize = 14.0f,
            .text = truncateText(playlistDescription, maxHeaderDescriptionCharacters),
        },
        {
            .fill = iconGrey,
            .stroke = iconGrey,
            .strokeWidth = 0.0f,
        }));
    float headerActionX = headerActionsX;
    const auto addHeaderAction = [&](const std::string& icon, bool primary = false) {
        addHeaderActionButton(icon, headerActionX, headerActionsY, headerActionSize, primary);
        headerActionX += headerActionSize + headerActionGap;
    };
    if (!allSongsSelected) {
        addHeaderAction(pinIcon);
        addHeaderAction(renameIcon);
        addHeaderAction(exportIcon);
    }
    addHeaderAction(shuffleIcon);
    addHeaderAction(playIcon, true);

    for (std::size_t statIndex = 0; statIndex < playlistStats.size(); ++statIndex) {
        const std::size_t column = statIndex % headerStatColumnCount;
        const std::size_t row = statIndex / headerStatColumnCount;
        const float statX = contentX + headerPadding
            + static_cast<float>(column) * (headerStatWidth + headerStatGap);
        const float statY = contentY + headerStatsTop
            + static_cast<float>(row) * (headerStatHeight + headerStatGap);
        const std::size_t maxStatValueCharacters = static_cast<std::size_t>(std::max(4.0f, headerStatWidth / 8.0f));

        primitives_.add(Primitive::roundedRect(
            {
                .x = statX,
                .y = statY,
                .width = headerStatWidth,
                .height = headerStatHeight,
                .radius = 0.0f,
            },
            {
                .fill = mantle,
                .stroke = border,
                .strokeWidth = 1.0f,
            }));
        primitives_.add(Primitive::text(
            {
                .x = statX + 10.0f,
                .y = statY + 7.0f,
                .fontSize = 9.0f,
                .text = std::string(playlistStats[statIndex].first),
            },
            {
                .fill = iconGrey,
                .stroke = iconGrey,
                .strokeWidth = 0.0f,
            }));
        primitives_.add(Primitive::text(
            {
                .x = statX + 10.0f,
                .y = statY + 22.0f,
                .fontSize = 14.0f,
                .text = truncateText(playlistStats[statIndex].second, maxStatValueCharacters),
            },
            {
                .fill = statIndex == 0 ? accent : sidebarText,
                .stroke = statIndex == 0 ? accent : sidebarText,
                .strokeWidth = 0.0f,
            }));
    }

    float trackY = contentY + headerHeight + 18.0f;
    std::size_t visibleTrackCount = 0;
    const auto addTrackRow = [&](const Track& track) {
        if (trackY + rowHeight > contentBottom) {
            return false;
        }

        primitives_.add(Primitive::roundedRect(
            {
                .x = contentX,
                .y = trackY,
                .width = contentWidth,
                .height = rowHeight,
                .radius = 0.0f,
            },
            {
                .fill = visibleTrackCount % 2 == 0 ? rgb(43, 51, 56, 0.42f) : rgb(52, 63, 68, 0.24f),
                .stroke = transparent,
                .strokeWidth = 0.0f,
            }));
        primitives_.add(Primitive::text(
            {
                .x = contentX + 12.0f,
                .y = trackY + 8.0f,
                .fontSize = 15.0f,
                .text = truncateText(track.title, maxTitleCharacters),
            },
            {
                .fill = sidebarText,
                .stroke = sidebarText,
                .strokeWidth = 0.0f,
            }));

        trackY += rowHeight + 4.0f;
        ++visibleTrackCount;
        return true;
    };

    if (selectedPlaylist != nullptr) {
        for (const std::size_t trackIndex : selectedPlaylist->trackIndexes) {
            if (trackIndex < tracks_.size() && !addTrackRow(tracks_[trackIndex])) {
                break;
            }
        }
    } else {
        for (const Track& track : tracks_) {
            if (!addTrackRow(track)) {
                break;
            }
        }
    }

    if (visibleTrackCount == 0) {
        primitives_.add(Primitive::text(
            {
                .x = contentX,
                .y = trackY + 8.0f,
                .fontSize = 15.0f,
                .text = "No songs in this playlist",
            },
            {
                .fill = iconGrey,
                .stroke = iconGrey,
                .strokeWidth = 0.0f,
            }));
    }

    addPlaylistButton(0, "All Songs", playlistIcon, sidebarPadding);
    primitives_.add(Primitive::line(
        {
            .x0 = sidebarPadding,
            .y0 = sidebarPadding + playlistButtonHeight + sidebarPadding,
            .x1 = sidebarWidth - sidebarPadding,
            .y1 = sidebarPadding + playlistButtonHeight + sidebarPadding,
            .thickness = 1.0f,
        },
        {
            .fill = separator,
            .stroke = separator,
            .strokeWidth = 0.0f,
        }));

    float playlistY = sidebarPadding + playlistButtonHeight + sidebarPadding + playlistButtonGap;
    for (const Playlist& playlist : playlists_) {
        addPlaylistButton(playlist.id, playlist.name, playlistIcon, playlistY);
        playlistY += playlistButtonHeight + playlistButtonGap;
    }

    const float helpY = windowHeight - sidebarPadding - sidebarButtonHeight;
    const float addSongsY = helpY - sidebarButtonGap - sidebarButtonHeight;
    primitives_.add(Primitive::line(
        {
            .x0 = sidebarPadding,
            .y0 = addSongsY - sidebarPadding,
            .x1 = sidebarWidth - sidebarPadding,
            .y1 = addSongsY - sidebarPadding,
            .thickness = 1.0f,
        },
        {
            .fill = separator,
            .stroke = separator,
            .strokeWidth = 0.0f,
        }));
    addSidebarButton("Import Songs", addSongsIcon, addSongsY, [this]() {
        toggleAddSongsMenu();
    });
    addSidebarButton("Settings", settingsIcon, helpY);

    if (addSongsMenuOpen_) {
        constexpr float menuPadding = 20.0f;
        constexpr float menuButtonHeight = 44.0f;
        constexpr float closeButtonSize = 30.0f;
        constexpr float closeIconSize = 18.0f;
        const Rect menu = addSongsMenuRect(
            windowWidth,
            windowHeight,
            addSongsDirectoryOptionsVisible_,
            addSongsPlaylistDropdownOpen_,
            playlists_.size());
        const Color menuBackground = rgb(52, 63, 68);
        const Color menuSubtleText = rgb(133, 146, 137);
        const Color dimOverlay = rgb(30, 35, 38, 0.72f);
        const Color primaryButtonFill = rgb(55, 65, 69);
        const Color primaryButtonHoverFill = rgb(65, 75, 80);
        const Color primaryButtonPressedFill = rgb(73, 81, 86);
        const Color listBackground = rgb(43, 52, 56);
        const Color listRowFill = rgb(49, 59, 63);
        const Color selectedPlaylistFill = rgb(60, 72, 65);
        const auto addMenuButton = [&](std::string label, float y, std::function<void()> onClick = {}, bool selected = false, bool primary = false) {
            primitives_.add(Primitive::button(
                {
                    .x = menu.x + menuPadding,
                    .y = y,
                    .width = menu.width - menuPadding * 2.0f,
                    .height = menuButtonHeight,
                    .padding = 12.0f,
                    .radius = 0.0f,
                    .fontSize = 15.0f,
                    .label = std::move(label),
                    .labelColor = sidebarText,
                    .hoverLabelColor = (primary || selected) ? sidebarText : accent,
                    .pressedLabelColor = sidebarText,
                    .hoverFill = (primary || selected) ? primaryButtonHoverFill : mantle,
                    .pressedFill = (primary || selected) ? primaryButtonPressedFill : activeFill,
                    .hoverStroke = (primary || selected) ? accent : border,
                    .pressedStroke = accent,
                    .onClick = std::move(onClick),
                    .centerLabel = false,
                },
                {
                    .fill = primary ? primaryButtonFill : selected ? activeFill : transparent,
                    .stroke = (selected || primary) ? accent : transparent,
                    .strokeWidth = 1.0f,
                }));
        };

        primitives_.add(Primitive::quad(
            {
                .x = 0.0f,
                .y = 0.0f,
                .width = windowWidth,
                .height = windowHeight,
            },
            {
                .fill = dimOverlay,
                .stroke = dimOverlay,
                .strokeWidth = 0.0f,
            }));
        primitives_.add(Primitive::roundedRect(
            {
                .x = menu.x,
                .y = menu.y,
                .width = menu.width,
                .height = menu.height,
                .radius = 0.0f,
            },
            {
                .fill = menuBackground,
                .stroke = accent,
                .strokeWidth = 1.0f,
            }));
        primitives_.add(Primitive::text(
            {
                .x = menu.x + menuPadding,
                .y = menu.y + 24.0f,
                .fontSize = 19.0f,
                .text = "Import Songs",
            },
            {
                .fill = sidebarText,
                .stroke = sidebarText,
                .strokeWidth = 0.0f,
            }));
        primitives_.add(Primitive::button(
            {
                .x = menu.x + menu.width - menuPadding - closeButtonSize,
                .y = menu.y + 16.0f,
                .width = closeButtonSize,
                .height = closeButtonSize,
                .padding = (closeButtonSize - closeIconSize) * 0.5f,
                .radius = 0.0f,
                .iconSize = closeIconSize,
                .label = "",
                .iconSvg = closeIcon,
                .iconColor = sidebarText,
                .hoverFill = mantle,
                .pressedFill = activeFill,
                .hoverStroke = border,
                .pressedStroke = accent,
                .onClick = [this]() {
                    closeAddSongsMenu();
                },
            },
            {
                .fill = transparent,
                .stroke = transparent,
                .strokeWidth = 1.0f,
            }));

        float menuButtonY = menu.y + 64.0f;
        addMenuButton("Import File(s)", menuButtonY, [this]() {
            beginImportFiles();
        }, false, true);

        menuButtonY += menuButtonHeight + addSongsMenuButtonGap;
        const float playlistDropdownY = menuButtonY;
        const std::size_t playlistOptionCount = std::max<std::size_t>(1, playlists_.size());
        const float playlistDropdownHeight = menuButtonHeight
            + static_cast<float>(playlistOptionCount) * addSongsPlaylistOptionHeight;
        if (addSongsPlaylistDropdownOpen_) {
            primitives_.add(Primitive::roundedRect(
                {
                    .x = menu.x + menuPadding,
                    .y = playlistDropdownY,
                    .width = menu.width - menuPadding * 2.0f,
                    .height = playlistDropdownHeight,
                    .radius = 0.0f,
                },
                {
                    .fill = listBackground,
                    .stroke = transparent,
                    .strokeWidth = 0.0f,
                }));
        }
        addMenuButton("Select Playlists", playlistDropdownY, [this]() {
            toggleAddSongsPlaylistDropdown();
        }, addSongsPlaylistDropdownOpen_);
        constexpr float playlistChevronSize = 18.0f;
        primitives_.add(Primitive::svg(
            {
                .x = menu.x + menu.width - menuPadding - 12.0f - playlistChevronSize,
                .y = playlistDropdownY + (menuButtonHeight - playlistChevronSize) * 0.5f,
                .width = playlistChevronSize,
                .height = playlistChevronSize,
                .rasterScale = 4.0f,
                .source = addSongsPlaylistDropdownOpen_ ? chevronUpIcon : chevronDownIcon,
                .sourceType = SvgSourceType::Data,
                .renderMode = SvgRenderMode::Mask,
            },
            {
                .fill = sidebarText,
                .stroke = transparent,
                .strokeWidth = 0.0f,
            }));

        menuButtonY += menuButtonHeight + (addSongsPlaylistDropdownOpen_ ? 0.0f : addSongsMenuButtonGap);
        if (addSongsPlaylistDropdownOpen_) {
            const float firstPlaylistOptionY = menuButtonY;
            if (playlists_.empty()) {
                primitives_.add(Primitive::text(
                    {
                        .x = menu.x + menuPadding + 12.0f,
                        .y = menuButtonY + 9.0f,
                        .fontSize = 14.0f,
                        .text = "No playlists available",
                    },
                    {
                        .fill = menuSubtleText,
                        .stroke = menuSubtleText,
                        .strokeWidth = 0.0f,
                    }));
                menuButtonY += addSongsPlaylistOptionHeight;
            } else {
                for (const Playlist& playlist : playlists_) {
                    const bool selected = std::ranges::find(selectedAddSongsPlaylistIds_, playlist.id)
                        != selectedAddSongsPlaylistIds_.end();
                    primitives_.add(Primitive::button(
                        {
                            .x = menu.x + menuPadding,
                            .y = menuButtonY,
                            .width = menu.width - menuPadding * 2.0f,
                            .height = addSongsPlaylistOptionHeight,
                            .padding = 12.0f,
                            .radius = 0.0f,
                            .fontSize = 14.0f,
                            .label = playlist.name,
                            .labelColor = sidebarText,
                            .hoverLabelColor = selected ? sidebarText : accent,
                            .pressedLabelColor = sidebarText,
                            .hoverFill = selected ? selectedPlaylistFill : mantle,
                            .pressedFill = selected ? selectedPlaylistFill : activeFill,
                            .hoverStroke = transparent,
                            .pressedStroke = transparent,
                            .onClick = [this, id = playlist.id]() {
                                toggleAddSongsPlaylistSelection(id);
                            },
                            .centerLabel = false,
                        },
                        {
                            .fill = selected ? selectedPlaylistFill : listBackground,
                            .stroke = transparent,
                            .strokeWidth = 0.0f,
                        }));
                    if (selected) {
                        constexpr float playlistCheckSize = 16.0f;
                        primitives_.add(Primitive::svg(
                            {
                                .x = menu.x + menu.width - menuPadding - 12.0f - playlistCheckSize,
                                .y = menuButtonY + (addSongsPlaylistOptionHeight - playlistCheckSize) * 0.5f,
                                .width = playlistCheckSize,
                                .height = playlistCheckSize,
                                .rasterScale = 4.0f,
                                .source = checkIcon,
                                .sourceType = SvgSourceType::Data,
                                .renderMode = SvgRenderMode::Mask,
                            },
                            {
                                .fill = accent,
                                .stroke = transparent,
                                .strokeWidth = 0.0f,
                            }));
                    }
                    menuButtonY += addSongsPlaylistOptionHeight;
                }
            }

            for (std::size_t optionIndex = 1; optionIndex < playlistOptionCount; ++optionIndex) {
                const float separatorY = firstPlaylistOptionY
                    + static_cast<float>(optionIndex) * addSongsPlaylistOptionHeight;
                primitives_.add(Primitive::line(
                    {
                        .x0 = menu.x + menuPadding + 1.0f,
                        .y0 = separatorY,
                        .x1 = menu.x + menu.width - menuPadding - 1.0f,
                        .y1 = separatorY,
                        .thickness = 1.0f,
                    },
                    {
                        .fill = separator,
                        .stroke = separator,
                        .strokeWidth = 0.0f,
                    }));
            }
            primitives_.add(Primitive::roundedRect(
                {
                    .x = menu.x + menuPadding,
                    .y = playlistDropdownY,
                    .width = menu.width - menuPadding * 2.0f,
                    .height = playlistDropdownHeight,
                    .radius = 0.0f,
                },
                {
                    .fill = transparent,
                    .stroke = accent,
                    .strokeWidth = 1.0f,
                }));
            menuButtonY += addSongsMenuButtonGap;
        }

        if (addSongsDirectoryOptionsVisible_) {
            addMenuButton(
                directoryImportMode_ == DirectoryImportMode::Playlist ? "Import as Playlist" : "Import Songs In Playlist",
                menuButtonY,
                [this]() {
                    setDirectoryImportMode(
                        directoryImportMode_ == DirectoryImportMode::Playlist
                            ? DirectoryImportMode::Files
                            : DirectoryImportMode::Playlist);
                },
                true);
        }

        addMenuButton("Import", menu.y + menu.height - menuPadding - menuButtonHeight, [this]() {
            addPendingSongs();
        }, false, true);

        {
            const Rect panel = addSongsFullListRect(windowWidth, windowHeight, menu, pendingAddSongs_.size());
            if (panel.width > 0.0f) {
                primitives_.add(Primitive::roundedRect(
                    {
                        .x = panel.x,
                        .y = panel.y,
                        .width = panel.width,
                        .height = panel.height,
                        .radius = 0.0f,
                    },
                    {
                        .fill = menuBackground,
                        .stroke = accent,
                        .strokeWidth = 1.0f,
                    }));
                primitives_.add(Primitive::text(
                    {
                        .x = panel.x + menuPadding,
                        .y = panel.y + 24.0f,
                        .fontSize = 17.0f,
                        .text = "Pending Songs",
                    },
                    {
                        .fill = sidebarText,
                        .stroke = sidebarText,
                        .strokeWidth = 0.0f,
                    }));
                const std::string pendingSongCountText = formatGroupedNumber(
                    pendingSongMatchCount(),
                    numberGroupingSeparator_) + " Songs";
                primitives_.add(Primitive::text(
                    {
                        .x = panel.x + panel.width - menuPadding - static_cast<float>(pendingSongCountText.size()) * 7.2f,
                        .y = panel.y + 26.0f,
                        .fontSize = 12.0f,
                        .text = pendingSongCountText,
                    },
                    {
                        .fill = menuSubtleText,
                        .stroke = menuSubtleText,
                        .strokeWidth = 0.0f,
                    }));

                const float searchY = panel.y + 58.0f;
                addSongsSearchFieldId_ = primitives_.add(Primitive::textField(
                    {
                        .x = panel.x + menuPadding,
                        .y = searchY,
                        .width = panel.width - menuPadding * 2.0f,
                        .height = 36.0f,
                        .radius = 0.0f,
                        .padding = 10.0f,
                        .fontSize = 14.0f,
                        .caretWidth = 1.0f,
                        .text = addSongsSearchQuery_,
                        .placeholder = "Search",
                        .textColor = sidebarText,
                        .placeholderColor = menuSubtleText,
                        .caretColor = sidebarText,
                        .caretCodepointIndex = addSongsSearchQuery_.size(),
                        .focused = addSongsSearchFocused_,
                        .caretVisible = addSongsCaretVisible_,
                    },
                    {
                        .fill = listBackground,
                        .stroke = addSongsSearchFocused_ ? accent : border,
                        .strokeWidth = 1.0f,
                    }));
                primitives_.add(Primitive::line(
                    {
                        .x0 = panel.x + menuPadding,
                        .y0 = searchY + 50.0f,
                        .x1 = panel.x + panel.width - menuPadding,
                        .y1 = searchY + 50.0f,
                        .thickness = 1.0f,
                    },
                    {
                        .fill = separator,
                        .stroke = separator,
                        .strokeWidth = 0.0f,
                    }));

                const Rect listViewport = pendingSongsListViewportRect(panel);
                const std::size_t matches = pendingSongMatchCount();
                pendingAddSongsScrollOffset_ = std::clamp(
                    pendingAddSongsScrollOffset_,
                    0.0f,
                    pendingSongsMaxScrollOffset(listViewport, matches));
                const std::size_t firstVisibleRow = static_cast<std::size_t>(
                    std::max(0.0f, std::floor(pendingAddSongsScrollOffset_ / addSongsFullListRowHeight)));
                const std::size_t visibleRowCapacity = pendingSongsVisibleRowCapacity(listViewport);
                const std::size_t lastVisibleRow = std::min(matches, firstVisibleRow + visibleRowCapacity);
                const std::size_t maxTitleCharacters = static_cast<std::size_t>(
                    std::max(18.0f, (panel.width - menuPadding * 2.0f - 20.0f) / 7.2f));

                if (matches > 0) {
                    primitives_.add(Primitive::roundedRect(
                        {
                            .x = listViewport.x,
                            .y = listViewport.y,
                            .width = listViewport.width,
                            .height = listViewport.height,
                            .radius = 0.0f,
                        },
                        {
                            .fill = listBackground,
                            .stroke = transparent,
                            .strokeWidth = 0.0f,
                        }));
                }

                for (std::size_t matchIndex = firstVisibleRow; matchIndex < lastVisibleRow; ++matchIndex) {
                    const std::size_t pendingIndex = normalizedAddSongsSearchQuery_.empty()
                        ? matchIndex
                        : filteredPendingSongIndexes_[matchIndex];
                    if (pendingIndex >= pendingAddSongs_.size()) {
                        continue;
                    }

                    const PendingAudioFile& song = pendingAddSongs_[pendingIndex];
                    const float rowY = listViewport.y + static_cast<float>(matchIndex) * addSongsFullListRowHeight - pendingAddSongsScrollOffset_;

                    primitives_.add(Primitive::roundedRect(
                        {
                            .x = listViewport.x,
                            .y = rowY,
                            .width = listViewport.width,
                            .height = addSongsFullListRowHeight,
                            .radius = 0.0f,
                        },
                        {
                            .fill = matchIndex % 2 == 0 ? listRowFill : listBackground,
                            .stroke = transparent,
                            .strokeWidth = 0.0f,
                        }));
                    if (matchIndex + 1 < lastVisibleRow) {
                        primitives_.add(Primitive::line(
                            {
                                .x0 = listViewport.x,
                                .y0 = rowY + addSongsFullListRowHeight,
                                .x1 = listViewport.x + listViewport.width,
                                .y1 = rowY + addSongsFullListRowHeight,
                                .thickness = 1.0f,
                            },
                            {
                                .fill = separator,
                                .stroke = separator,
                                .strokeWidth = 0.0f,
                            }));
                    }
                    primitives_.add(Primitive::text(
                        {
                            .x = listViewport.x + 9.0f,
                            .y = rowY + 7.0f,
                            .fontSize = 13.0f,
                            .text = truncateText(song.displayName, maxTitleCharacters),
                        },
                        {
                            .fill = sidebarText,
                            .stroke = sidebarText,
                            .strokeWidth = 0.0f,
                        }));
                }

                if (pendingAddSongs_.empty()) {
                    primitives_.add(Primitive::text(
                        {
                            .x = listViewport.x,
                            .y = listViewport.y + 4.0f,
                            .fontSize = 14.0f,
                            .text = "No pending songs",
                        },
                        {
                            .fill = menuSubtleText,
                            .stroke = menuSubtleText,
                            .strokeWidth = 0.0f,
                        }));
                } else if (matches == 0) {
                    primitives_.add(Primitive::text(
                        {
                            .x = listViewport.x,
                            .y = listViewport.y + 4.0f,
                            .fontSize = 14.0f,
                            .text = "No songs match",
                        },
                        {
                            .fill = menuSubtleText,
                            .stroke = menuSubtleText,
                            .strokeWidth = 0.0f,
                        }));
                }

                const PendingSongsScrollbar scrollbar = pendingSongsScrollbar(listViewport, matches, pendingAddSongsScrollOffset_);
                if (scrollbar.visible) {
                    primitives_.add(Primitive::roundedRect(
                        {
                            .x = scrollbar.track.x,
                            .y = scrollbar.track.y,
                            .width = scrollbar.track.width,
                            .height = scrollbar.track.height,
                            .radius = 0.0f,
                        },
                        {
                            .fill = border,
                            .stroke = transparent,
                            .strokeWidth = 0.0f,
                        }));
                    primitives_.add(Primitive::roundedRect(
                        {
                            .x = scrollbar.thumb.x,
                            .y = scrollbar.thumb.y,
                            .width = scrollbar.thumb.width,
                            .height = scrollbar.thumb.height,
                            .radius = 0.0f,
                        },
                        {
                            .fill = menuSubtleText,
                            .stroke = transparent,
                            .strokeWidth = 0.0f,
                        }));
                }

                if (matches > 0) {
                    primitives_.add(Primitive::roundedRect(
                        {
                            .x = listViewport.x,
                            .y = listViewport.y,
                            .width = listViewport.width,
                            .height = listViewport.height,
                            .radius = 0.0f,
                        },
                        {
                            .fill = transparent,
                            .stroke = border,
                            .strokeWidth = 1.0f,
                        }));
                }
            }
        }

        if (addSongsAudioScanActive_) {
            const AudioScanProgressSnapshot scanProgress = snapshotAudioScanProgress(pendingAudioScanProgress_);
            const Color scanOverlay = rgb(25, 29, 32, 0.68f);
            const Rect scanMenu = centeredFitRect(windowWidth, windowHeight, 460.0f, 174.0f, 320.0f, 154.0f, floatingMenuMargin);
            const float scanPadding = 22.0f;
            const float progressBarX = scanMenu.x + scanPadding;
            const float progressBarY = scanMenu.y + 116.0f;
            const float progressBarWidth = std::max(0.0f, scanMenu.width - scanPadding * 2.0f);
            const float progressBarHeight = 8.0f;
            const float scanProgressValue = audioScanProgressFraction(scanProgress);
            const std::size_t maxPathCharacters = static_cast<std::size_t>(
                std::max(24.0f, (scanMenu.width - scanPadding * 2.0f) / 7.0f));
            primitives_.add(Primitive::quad(
                {
                    .x = 0.0f,
                    .y = 0.0f,
                    .width = windowWidth,
                    .height = windowHeight,
                },
                {
                    .fill = scanOverlay,
                    .stroke = scanOverlay,
                    .strokeWidth = 0.0f,
                }));
            primitives_.add(Primitive::roundedRect(
                {
                    .x = scanMenu.x,
                    .y = scanMenu.y,
                    .width = scanMenu.width,
                    .height = scanMenu.height,
                    .radius = 0.0f,
                },
                {
                    .fill = menuBackground,
                    .stroke = accent,
                    .strokeWidth = 1.0f,
                }));
            primitives_.add(Primitive::text(
                {
                    .x = scanMenu.x + scanPadding,
                    .y = scanMenu.y + 30.0f,
                    .fontSize = 18.0f,
                    .text = "Scanning Folder",
                },
                {
                    .fill = sidebarText,
                    .stroke = sidebarText,
                    .strokeWidth = 0.0f,
                }));
            audioScanStatusTextId_ = primitives_.add(Primitive::text(
                {
                    .x = scanMenu.x + scanPadding,
                    .y = scanMenu.y + 62.0f,
                    .fontSize = 14.0f,
                    .text = audioScanStatusText(scanProgress),
                },
                {
                    .fill = menuSubtleText,
                    .stroke = menuSubtleText,
                    .strokeWidth = 0.0f,
                }));
            audioScanPathTextId_ = primitives_.add(Primitive::text(
                {
                    .x = scanMenu.x + scanPadding,
                    .y = scanMenu.y + 88.0f,
                    .fontSize = 12.0f,
                    .text = audioScanPathText(scanProgress, maxPathCharacters),
                },
                {
                    .fill = sidebarText,
                    .stroke = sidebarText,
                    .strokeWidth = 0.0f,
                }));
            primitives_.add(Primitive::roundedRect(
                {
                    .x = progressBarX,
                    .y = progressBarY,
                    .width = progressBarWidth,
                    .height = progressBarHeight,
                    .radius = 0.0f,
                },
                {
                    .fill = border,
                    .stroke = border,
                    .strokeWidth = 0.0f,
                }));
            audioScanProgressFillId_ = primitives_.add(Primitive::roundedRect(
                {
                    .x = progressBarX,
                    .y = progressBarY,
                    .width = progressBarWidth * scanProgressValue,
                    .height = progressBarHeight,
                    .radius = 0.0f,
                },
                {
                    .fill = accent,
                    .stroke = accent,
                    .strokeWidth = 0.0f,
                }));
        }
    }
}

App::App()
    : window_(1280, 720, "womp")
    , renderer_(window_)
{
    lastImportDirectory_ = loadLastImportDirectory();
    numberGroupingSeparator_ = loadNumberGroupingSeparator();
    addPlaylist("Recently Added");
    addPlaylist("Favorites");
    addPlaylist("Long Drives");
    rebuildScene();
    window_.setPointerEventHandler([this](const WaylandWindow::PointerEvent& event) {
        handlePointerEvent(event);
    });
    window_.setKeyEventHandler([this](const WaylandWindow::KeyEvent& event) {
        handleKeyEvent(event);
    });
}

void App::run()
{
    bool needsDraw = true;
    while (window_.pollEvents(eventPollTimeoutMilliseconds(needsDraw))) {
        completePendingAudioScanIfReady();
        updateAddSongsCaretBlink();

        if (window_.takeResizeFlag()) {
            renderer_.recreateSwapchain();
            pressedButton_ = 0;
            rebuildScene();
            needsDraw = true;
        }

        if (primitivesDirty_) {
            renderer_.setPrimitives(renderPrimitives(pendingPrimitiveUpdate_), pendingPrimitiveUpdate_);
            pendingPrimitiveUpdate_ = VulkanRenderer::PrimitiveUpdate::DrawOnly;
            primitivesDirty_ = false;
            needsDraw = true;
        }

        if (needsDraw) {
            renderer_.drawFrame();
            needsDraw = false;
        }
    }

    renderer_.waitIdle();
}

App::PlaylistId App::addPlaylist(std::string name)
{
    const PlaylistId id = nextPlaylistId_++;
    playlists_.push_back({
        .id = id,
        .name = std::move(name),
    });
    if (sceneReady_) {
        rebuildScene();
    }
    return id;
}

bool App::removePlaylist(PlaylistId id)
{
    if (id == 0) {
        return false;
    }

    const auto removed = std::erase_if(playlists_, [id](const Playlist& playlist) {
        return playlist.id == id;
    });
    if (removed == 0) {
        return false;
    }

    if (selectedPlaylistId_ == id) {
        selectedPlaylistId_ = 0;
    }
    std::erase(selectedAddSongsPlaylistIds_, id);

    rebuildScene();
    return true;
}

std::vector<Primitive> App::renderPrimitives(VulkanRenderer::PrimitiveUpdate update) const
{
    const bool needsText = includesUpdate(update, VulkanRenderer::PrimitiveUpdate::Text);
    const bool needsSvg = includesUpdate(update, VulkanRenderer::PrimitiveUpdate::Svg);
    std::vector<Primitive> result;
    result.reserve(primitives_.size());

    for (const Primitive& primitive : primitives_.all()) {
        if (!primitive.visible) {
            continue;
        }

        Primitive snapshot{
            .id = primitive.id,
            .style = primitive.style,
            .visible = primitive.visible,
        };

        if (const auto* roundedRect = std::get_if<RoundedRectPrimitive>(&primitive.geometry)) {
            snapshot.geometry = *roundedRect;
        } else if (const auto* circle = std::get_if<CirclePrimitive>(&primitive.geometry)) {
            snapshot.geometry = *circle;
        } else if (const auto* quad = std::get_if<QuadPrimitive>(&primitive.geometry)) {
            snapshot.geometry = *quad;
        } else if (const auto* triangle = std::get_if<TrianglePrimitive>(&primitive.geometry)) {
            snapshot.geometry = *triangle;
        } else if (const auto* line = std::get_if<LinePrimitive>(&primitive.geometry)) {
            snapshot.geometry = *line;
        } else if (const auto* text = std::get_if<TextPrimitive>(&primitive.geometry)) {
            snapshot.geometry = TextPrimitive{
                .x = text->x,
                .y = text->y,
                .fontSize = text->fontSize,
                .text = needsText ? text->text : std::string{},
                .fontFamilies = needsText ? text->fontFamilies : std::vector<std::string>{},
            };
        } else if (const auto* svg = std::get_if<SvgPrimitive>(&primitive.geometry)) {
            snapshot.geometry = SvgPrimitive{
                .x = svg->x,
                .y = svg->y,
                .width = svg->width,
                .height = svg->height,
                .rasterScale = svg->rasterScale,
                .sdfSpread = svg->sdfSpread,
                .source = needsSvg ? svg->source : cachedSourceMarker(svg->source),
                .sourceType = svg->sourceType,
                .renderMode = svg->renderMode,
            };
        } else if (const auto* textField = std::get_if<TextFieldPrimitive>(&primitive.geometry)) {
            snapshot.geometry = TextFieldPrimitive{
                .x = textField->x,
                .y = textField->y,
                .width = textField->width,
                .height = textField->height,
                .radius = textField->radius,
                .padding = textField->padding,
                .fontSize = textField->fontSize,
                .caretWidth = textField->caretWidth,
                .text = needsText ? textField->text : (textField->text.empty() ? std::string{} : std::string{"cached"}),
                .placeholder = needsText ? textField->placeholder : (textField->placeholder.empty() ? std::string{} : std::string{"cached"}),
                .fontFamilies = needsText ? textField->fontFamilies : std::vector<std::string>{},
                .textColor = textField->textColor,
                .placeholderColor = textField->placeholderColor,
                .caretColor = textField->caretColor,
                .caretCodepointIndex = textField->caretCodepointIndex,
                .focused = textField->focused,
                .caretVisible = textField->caretVisible,
            };
        } else if (const auto* button = std::get_if<ButtonPrimitive>(&primitive.geometry)) {
            snapshot.geometry = ButtonPrimitive{
                .x = button->x,
                .y = button->y,
                .width = button->width,
                .height = button->height,
                .padding = button->padding,
                .radius = button->radius,
                .fontSize = button->fontSize,
                .iconSize = button->iconSize,
                .iconGap = button->iconGap,
                .label = needsText ? button->label : std::string{},
                .iconSvg = needsSvg ? button->iconSvg : cachedSourceMarker(button->iconSvg),
                .fontFamilies = needsText ? button->fontFamilies : std::vector<std::string>{},
                .iconColor = button->iconColor,
                .labelColor = button->labelColor,
                .hoverLabelColor = button->hoverLabelColor,
                .pressedLabelColor = button->pressedLabelColor,
                .disabledLabelColor = button->disabledLabelColor,
                .hoverFill = button->hoverFill,
                .pressedFill = button->pressedFill,
                .disabledFill = button->disabledFill,
                .hoverStroke = button->hoverStroke,
                .pressedStroke = button->pressedStroke,
                .hovered = button->hovered,
                .pressed = button->pressed,
                .enabled = button->enabled,
                .centerLabel = button->centerLabel,
            };
        }

        result.push_back(std::move(snapshot));
    }

    return result;
}

void App::selectPlaylist(PlaylistId id)
{
    if (selectedPlaylistId_ == id) {
        return;
    }

    selectedPlaylistId_ = id;
    rebuildScene();
}

bool App::addTrackToPlaylist(PlaylistId playlistId, std::size_t trackIndex)
{
    if (playlistId == 0 || trackIndex >= tracks_.size()) {
        return false;
    }

    const auto playlist = std::ranges::find(playlists_, playlistId, &Playlist::id);
    if (playlist == playlists_.end() || !playlist->trackIndexSet.insert(trackIndex).second) {
        return false;
    }

    playlist->trackIndexes.push_back(trackIndex);
    return true;
}

void App::resetPendingSongs()
{
    pendingAddSongs_ = {};
    filteredPendingSongIndexes_ = {};
}

void App::resetAddSongsMenuState(bool clearPendingSongs)
{
    addSongsDirectoryOptionsVisible_ = false;
    addSongsPlaylistDropdownOpen_ = false;
    addSongsSearchFocused_ = false;
    addSongsCaretVisible_ = true;
    nextAddSongsCaretBlink_ = std::chrono::steady_clock::now() + addSongsCaretBlinkInterval;
    draggingPendingSongsScrollbar_ = false;
    pendingSongsScrollbarDragOffsetY_ = 0.0f;
    pendingAddSongsScrollOffset_ = 0.0f;
    if (clearPendingSongs) {
        resetPendingSongs();
        selectedAddSongsPlaylistIds_.clear();
        addSongsSearchQuery_.clear();
        normalizedAddSongsSearchQuery_.clear();
    } else {
        if (selectedAddSongsPlaylistIds_.empty() && selectedPlaylistId_ != 0) {
            selectedAddSongsPlaylistIds_.push_back(selectedPlaylistId_);
        }
        setAddSongsSearchQuery({});
    }
}

void App::setAddSongsSearchQuery(std::string query)
{
    if (addSongsSearchQuery_ == query) {
        return;
    }

    addSongsSearchQuery_ = std::move(query);
    rebuildPendingSongFilter();
}

void App::rebuildPendingSongFilter()
{
    const std::string previousQuery = normalizedAddSongsSearchQuery_;
    const std::string normalizedQuery = lowercaseAscii(addSongsSearchQuery_);
    const bool canNarrowExistingFilter = !previousQuery.empty()
        && normalizedQuery != previousQuery
        && normalizedQuery.starts_with(previousQuery)
        && !filteredPendingSongIndexes_.empty();
    std::vector<std::size_t> previousFilteredIndexes;
    if (canNarrowExistingFilter) {
        previousFilteredIndexes = std::move(filteredPendingSongIndexes_);
    }

    normalizedAddSongsSearchQuery_ = normalizedQuery;
    filteredPendingSongIndexes_.clear();
    draggingPendingSongsScrollbar_ = false;
    pendingAddSongsScrollOffset_ = 0.0f;

    if (normalizedAddSongsSearchQuery_.empty()) {
        return;
    }

    if (canNarrowExistingFilter) {
        filteredPendingSongIndexes_.reserve(previousFilteredIndexes.size());
        for (const std::size_t pendingIndex : previousFilteredIndexes) {
            if (pendingIndex < pendingAddSongs_.size()
                && pendingAddSongs_[pendingIndex].normalizedDisplayName.find(normalizedAddSongsSearchQuery_) != std::string::npos) {
                filteredPendingSongIndexes_.push_back(pendingIndex);
            }
        }
        return;
    }

    filteredPendingSongIndexes_.reserve(pendingAddSongs_.size());
    for (std::size_t pendingIndex = 0; pendingIndex < pendingAddSongs_.size(); ++pendingIndex) {
        if (pendingAddSongs_[pendingIndex].normalizedDisplayName.find(normalizedAddSongsSearchQuery_) != std::string::npos) {
            filteredPendingSongIndexes_.push_back(pendingIndex);
        }
    }
}

std::size_t App::pendingSongMatchCount() const
{
    return normalizedAddSongsSearchQuery_.empty()
        ? pendingAddSongs_.size()
        : filteredPendingSongIndexes_.size();
}

void App::resetAddSongsCaretBlink()
{
    addSongsCaretVisible_ = true;
    nextAddSongsCaretBlink_ = std::chrono::steady_clock::now() + addSongsCaretBlinkInterval;

    if (Primitive* primitive = primitives_.find(addSongsSearchFieldId_)) {
        if (auto* textField = std::get_if<TextFieldPrimitive>(&primitive->geometry); textField != nullptr && !textField->caretVisible) {
            textField->caretVisible = true;
            refreshPrimitives();
        }
    }
}

void App::updateAddSongsCaretBlink()
{
    if (!addSongsMenuOpen_ || !addSongsSearchFocused_ || addSongsAudioScanActive_) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now < nextAddSongsCaretBlink_) {
        return;
    }

    addSongsCaretVisible_ = !addSongsCaretVisible_;
    nextAddSongsCaretBlink_ = now + addSongsCaretBlinkInterval;
    if (Primitive* primitive = primitives_.find(addSongsSearchFieldId_)) {
        if (auto* textField = std::get_if<TextFieldPrimitive>(&primitive->geometry)) {
            textField->caretVisible = addSongsCaretVisible_;
            refreshPrimitives();
        }
    }
}

std::int32_t App::eventPollTimeoutMilliseconds(bool needsDraw) const
{
    if (addSongsAudioScanActive_) {
        return 16;
    }
    if (needsDraw) {
        return 0;
    }
    if (!addSongsMenuOpen_ || !addSongsSearchFocused_) {
        return -1;
    }

    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        nextAddSongsCaretBlink_ - std::chrono::steady_clock::now());
    return remaining.count() <= 0
        ? 0
        : static_cast<std::int32_t>(remaining.count() + 1);
}

void App::refreshAudioScanProgress()
{
    if (!addSongsAudioScanActive_) {
        return;
    }

    const AudioScanProgressSnapshot scanProgress = snapshotAudioScanProgress(pendingAudioScanProgress_);
    bool textChanged = false;

    if (Primitive* primitive = primitives_.find(audioScanStatusTextId_)) {
        if (auto* text = std::get_if<TextPrimitive>(&primitive->geometry)) {
            const std::string status = audioScanStatusText(scanProgress);
            if (text->text != status) {
                text->text = status;
                textChanged = true;
            }
        }
    }

    if (Primitive* primitive = primitives_.find(audioScanPathTextId_)) {
        if (auto* text = std::get_if<TextPrimitive>(&primitive->geometry)) {
            const Rect scanMenu = centeredFitRect(
                static_cast<float>(window_.width()),
                static_cast<float>(window_.height()),
                460.0f,
                174.0f,
                320.0f,
                154.0f,
                floatingMenuMargin);
            const std::size_t maxPathCharacters = static_cast<std::size_t>(
                std::max(24.0f, (scanMenu.width - 44.0f) / 7.0f));
            const std::string pathText = audioScanPathText(scanProgress, maxPathCharacters);
            if (text->text != pathText) {
                text->text = pathText;
                textChanged = true;
            }
        }
    }

    if (Primitive* primitive = primitives_.find(audioScanProgressFillId_)) {
        if (auto* fill = std::get_if<RoundedRectPrimitive>(&primitive->geometry)) {
            const float progress = audioScanProgressFraction(scanProgress);
            const Rect scanMenu = centeredFitRect(
                static_cast<float>(window_.width()),
                static_cast<float>(window_.height()),
                460.0f,
                174.0f,
                320.0f,
                154.0f,
                floatingMenuMargin);
            fill->width = std::max(0.0f, scanMenu.width - 44.0f) * progress;
        }
    }

    refreshPrimitives(textChanged ? VulkanRenderer::PrimitiveUpdate::Text : VulkanRenderer::PrimitiveUpdate::DrawOnly);
}

void App::toggleAddSongsMenu()
{
    addSongsMenuOpen_ = !addSongsMenuOpen_;
    if (!addSongsMenuOpen_) {
        resetAddSongsMenuState(true);
    } else {
        resetAddSongsMenuState(false);
    }
    rebuildScene();
}

void App::closeAddSongsMenu()
{
    if (!addSongsMenuOpen_) {
        return;
    }

    addSongsMenuOpen_ = false;
    resetAddSongsMenuState(true);
    rebuildScene();
}

void App::beginImportFiles()
{
    std::vector<std::filesystem::path> paths = runFileImportDialog(lastImportDirectory_);
    updateLastImportDirectory(paths);
    startPendingAudioScan(std::move(paths));
}

void App::completePendingAudioScanIfReady()
{
    if (!addSongsAudioScanActive_ || !pendingAudioScan_.valid()) {
        return;
    }

    if (pendingAudioScan_.wait_for(std::chrono::seconds{0}) == std::future_status::ready) {
        addSongsAudioScanActive_ = false;
        addPendingAudioFiles(pendingAudioScan_.get());
        pendingAudioScanProgress_.reset();
        renderedAudioScanProgressRevision_ = 0;
        return;
    }

    const AudioScanProgressSnapshot scanProgress = snapshotAudioScanProgress(pendingAudioScanProgress_);
    if (scanProgress.revision != renderedAudioScanProgressRevision_) {
        renderedAudioScanProgressRevision_ = scanProgress.revision;
        refreshAudioScanProgress();
    }
}

void App::addPendingSongs()
{
    if (pendingAddSongs_.empty()) {
        return;
    }

    std::vector<PendingAudioFile> songs = std::move(pendingAddSongs_);
    std::vector<PlaylistId> playlistIds = selectedAddSongsPlaylistIds_;
    resetAddSongsMenuState(true);
    importPendingSongs(std::move(songs), std::move(playlistIds));
}

void App::importPendingSongs(std::vector<PendingAudioFile> songs, std::vector<PlaylistId> playlistIds)
{
    if (songs.empty()) {
        return;
    }

    const std::size_t newTrackCount = std::ranges::count_if(songs, [this](const PendingAudioFile& song) {
        return !trackIndexesByPath_.contains(song.path);
    });
    tracks_.reserve(tracks_.size() + newTrackCount);
    trackIndexesByPath_.reserve(trackIndexesByPath_.size() + newTrackCount);

    bool imported = false;
    for (PendingAudioFile& song : songs) {
        const auto existingTrackIndex = trackIndexesByPath_.find(song.path);
        std::size_t trackIndex = 0;
        if (existingTrackIndex == trackIndexesByPath_.end()) {
            trackIndex = tracks_.size();
            trackIndexesByPath_.emplace(song.path, trackIndex);
            tracks_.push_back({
                .path = std::move(song.path),
                .title = std::move(song.displayName),
            });
            imported = true;
        } else {
            trackIndex = existingTrackIndex->second;
        }

        for (const PlaylistId playlistId : playlistIds) {
            imported = addTrackToPlaylist(playlistId, trackIndex) || imported;
        }
    }

    addSongsMenuOpen_ = false;
    resetAddSongsMenuState(true);
    if (!imported) {
        rebuildScene();
        return;
    }

    hasCurrentSong_ = true;
    currentSongElapsedSeconds_ = 0.0f;
    if (currentSongDurationSeconds_ <= 0.0f) {
        currentSongDurationSeconds_ = 180.0f;
    }
    rebuildScene();
}

void App::importFiles(const std::vector<std::filesystem::path>& paths)
{
    if (paths.empty()) {
        return;
    }

    bool imported = false;
    const auto addTrack = [&](const std::filesystem::path& path, PlaylistId playlistId) {
        const std::filesystem::path normalizedPath = std::filesystem::absolute(path).lexically_normal();
        if (!std::filesystem::is_regular_file(normalizedPath) || !isAudioFile(normalizedPath)) {
            return false;
        }

        const auto existingTrackIndex = trackIndexesByPath_.find(normalizedPath);
        std::size_t trackIndex = 0;
        bool addedTrack = false;
        if (existingTrackIndex != trackIndexesByPath_.end()) {
            trackIndex = existingTrackIndex->second;
        } else {
            trackIndex = tracks_.size();
            trackIndexesByPath_.emplace(normalizedPath, trackIndex);
            tracks_.push_back({
                .path = normalizedPath,
                .title = displayNameForPath(normalizedPath),
            });
            addedTrack = true;
        }

        return addTrackToPlaylist(playlistId, trackIndex) || addedTrack;
    };

    for (const std::filesystem::path& path : paths) {
        const std::filesystem::path normalizedPath = std::filesystem::absolute(path).lexically_normal();
        if (std::filesystem::is_directory(normalizedPath)) {
            const std::vector<std::filesystem::path> audioFiles = audioFilesInDirectory(normalizedPath);
            if (audioFiles.empty()) {
                continue;
            }

            if (directoryImportMode_ == DirectoryImportMode::Playlist) {
                const PlaylistId playlistId = addPlaylist(displayNameForPath(normalizedPath));
                for (const std::filesystem::path& audioFile : audioFiles) {
                    imported = addTrack(audioFile, playlistId) || imported;
                }
            } else {
                for (const std::filesystem::path& audioFile : audioFiles) {
                    imported = addTrack(audioFile, selectedPlaylistId_) || imported;
                }
            }
            continue;
        }

        imported = addTrack(normalizedPath, selectedPlaylistId_) || imported;
    }

    addSongsMenuOpen_ = false;
    resetAddSongsMenuState(true);
    if (!imported) {
        rebuildScene();
        return;
    }

    hasCurrentSong_ = true;
    currentSongElapsedSeconds_ = 0.0f;
    if (currentSongDurationSeconds_ <= 0.0f) {
        currentSongDurationSeconds_ = 180.0f;
    }
    rebuildScene();
}

void App::startPendingAudioScan(std::vector<std::filesystem::path> paths)
{
    if (paths.empty()) {
        return;
    }

    auto scanProgress = std::make_shared<AudioScanProgress>();
    updateAudioScanProgress(scanProgress, [&](AudioScanProgress& state) {
        state.phase = AudioScanProgress::Phase::Preparing;
        state.totalRoots = paths.size();
    });

    addSongsMenuOpen_ = true;
    addSongsAudioScanActive_ = true;
    resetAddSongsMenuState(false);
    pendingAudioScanProgress_ = scanProgress;
    renderedAudioScanProgressRevision_ = 0;
    pendingAudioScan_ = std::async(std::launch::async, [paths = std::move(paths), scanProgress]() {
        std::vector<std::filesystem::path> audioPaths = expandAudioImportPaths(paths, scanProgress);
        std::vector<PendingAudioFile> audioFiles;
        audioFiles.reserve(audioPaths.size());
        for (std::filesystem::path& path : audioPaths) {
            audioFiles.push_back(makePendingAudioFile(std::move(path)));
        }
        return audioFiles;
    });
    rebuildScene();
}

void App::addPendingAudioFiles(std::vector<PendingAudioFile> songs)
{
    if (songs.empty()) {
        rebuildScene();
        return;
    }

    songs.erase(std::ranges::unique(songs, pendingAudioFilePathEqual).begin(), songs.end());

    bool added = !songs.empty();
    if (pendingAddSongs_.empty()) {
        pendingAddSongs_ = std::move(songs);
    } else {
        const std::size_t previousCount = pendingAddSongs_.size();
        std::vector<PendingAudioFile> merged;
        merged.reserve(pendingAddSongs_.size() + songs.size());

        std::size_t pendingIndex = 0;
        std::size_t songIndex = 0;
        while (pendingIndex < pendingAddSongs_.size() && songIndex < songs.size()) {
            if (pendingAudioFilePathLess(pendingAddSongs_[pendingIndex], songs[songIndex])) {
                merged.push_back(std::move(pendingAddSongs_[pendingIndex++]));
            } else if (pendingAudioFilePathLess(songs[songIndex], pendingAddSongs_[pendingIndex])) {
                merged.push_back(std::move(songs[songIndex++]));
            } else {
                merged.push_back(std::move(pendingAddSongs_[pendingIndex++]));
                ++songIndex;
            }
        }

        while (pendingIndex < pendingAddSongs_.size()) {
            merged.push_back(std::move(pendingAddSongs_[pendingIndex++]));
        }
        while (songIndex < songs.size()) {
            merged.push_back(std::move(songs[songIndex++]));
        }

        added = merged.size() > previousCount;
        pendingAddSongs_ = std::move(merged);
    }

    addSongsDirectoryOptionsVisible_ = false;
    rebuildPendingSongFilter();
    if (added) {
        addSongsMenuOpen_ = true;
    }
    rebuildScene();
}

void App::updateLastImportDirectory(const std::vector<std::filesystem::path>& paths)
{
    for (const std::filesystem::path& path : paths) {
        std::error_code error;
        const std::filesystem::path normalizedPath = std::filesystem::absolute(path, error).lexically_normal();
        if (error || (!std::filesystem::is_regular_file(normalizedPath, error) && !std::filesystem::is_directory(normalizedPath, error))) {
            continue;
        }

        const std::filesystem::path directory = normalizedPath.parent_path();
        if (!directory.empty() && std::filesystem::is_directory(directory, error)) {
            lastImportDirectory_ = directory;
            saveLastImportDirectory(lastImportDirectory_);
            return;
        }
    }
}

void App::setDirectoryImportMode(DirectoryImportMode mode)
{
    if (directoryImportMode_ == mode) {
        return;
    }

    directoryImportMode_ = mode;
    rebuildScene();
}

void App::toggleAddSongsPlaylistDropdown()
{
    addSongsPlaylistDropdownOpen_ = !addSongsPlaylistDropdownOpen_;
    rebuildScene();
}

void App::toggleAddSongsPlaylistSelection(PlaylistId id)
{
    const auto selected = std::ranges::find(selectedAddSongsPlaylistIds_, id);
    if (selected == selectedAddSongsPlaylistIds_.end()) {
        selectedAddSongsPlaylistIds_.push_back(id);
    } else {
        selectedAddSongsPlaylistIds_.erase(selected);
    }
    rebuildScene();
}

bool App::addSongsMenuContains(float x, float y) const
{
    const Rect menu = addSongsMenuRect(
        static_cast<float>(window_.width()),
        static_cast<float>(window_.height()),
        addSongsDirectoryOptionsVisible_,
        addSongsPlaylistDropdownOpen_,
        playlists_.size());
    const bool inMenu = x >= menu.x
        && x <= menu.x + menu.width
        && y >= menu.y
        && y <= menu.y + menu.height;
    if (inMenu) {
        return inMenu;
    }

    const Rect panel = addSongsFullListRect(
        static_cast<float>(window_.width()),
        static_cast<float>(window_.height()),
        menu,
        pendingAddSongs_.size());
    return x >= panel.x
        && x <= panel.x + panel.width
        && y >= panel.y
        && y <= panel.y + panel.height;
}

bool App::addSongsSearchFieldContains(float x, float y) const
{
    if (!addSongsMenuOpen_ || addSongsAudioScanActive_) {
        return false;
    }

    const Rect menu = addSongsMenuRect(
        static_cast<float>(window_.width()),
        static_cast<float>(window_.height()),
        addSongsDirectoryOptionsVisible_,
        addSongsPlaylistDropdownOpen_,
        playlists_.size());
    const Rect panel = addSongsFullListRect(
        static_cast<float>(window_.width()),
        static_cast<float>(window_.height()),
        menu,
        pendingAddSongs_.size());
    if (panel.width <= 0.0f) {
        return false;
    }
    const TextFieldPrimitive searchField{
        .x = panel.x + 20.0f,
        .y = panel.y + 58.0f,
        .width = panel.width - 40.0f,
        .height = 36.0f,
    };
    return contains(searchField, x, y);
}

void App::togglePlayback()
{
    if (!hasCurrentSong_) {
        return;
    }

    playing_ = !playing_;
    rebuildScene();
}

void App::toggleShuffle()
{
    shuffleEnabled_ = !shuffleEnabled_;
    if (Primitive* primitive = primitives_.find(shuffleButtonId_)) {
        if (auto* button = std::get_if<ButtonPrimitive>(&primitive->geometry)) {
            button->iconColor = shuffleEnabled_ ? rgb(167, 192, 128) : rgb(133, 146, 137);
            refreshPrimitives();
            return;
        }
    }

    rebuildScene();
}

void App::toggleVolumeMute()
{
    if (volumeMuted_ || volume_ <= 0.0f) {
        volumeMuted_ = false;
        volume_ = volumeBeforeMute_ > 0.0f ? volumeBeforeMute_ : 1.0f;
    } else {
        volumeBeforeMute_ = volume_;
        volumeMuted_ = true;
    }

    refreshVolumeControl();
}

void App::setVolumeFromPointer(float x)
{
    volume_ = std::clamp((x - volumeSliderX_) / volumeSliderWidth_, 0.0f, 1.0f);
    if (volume_ > 0.0f) {
        volumeBeforeMute_ = volume_;
    }
    volumeMuted_ = false;
    refreshVolumeControl();
}

void App::setMediaProgressFromPointer(float x)
{
    if (!canSeekMediaProgress()) {
        return;
    }

    const float progress = std::clamp((x - mediaProgressSliderX_) / mediaProgressSliderWidth_, 0.0f, 1.0f);
    currentSongElapsedSeconds_ = currentSongDurationSeconds_ * progress;
    refreshMediaProgressControl();
}

bool App::mediaProgressSliderContains(float x, float y) const
{
    const float verticalPadding = std::max(0.0f, (mediaProgressSliderHitHeight_ - mediaProgressSliderHeight_) * 0.5f);
    return x >= mediaProgressSliderX_
        && x <= mediaProgressSliderX_ + mediaProgressSliderWidth_
        && y >= mediaProgressSliderY_ - verticalPadding
        && y <= mediaProgressSliderY_ + mediaProgressSliderHeight_ + verticalPadding;
}

bool App::volumeSliderContains(float x, float y) const
{
    const float verticalPadding = std::max(0.0f, (volumeSliderHitHeight_ - volumeSliderHeight_) * 0.5f);
    return x >= volumeSliderX_
        && x <= volumeSliderX_ + volumeSliderWidth_
        && y >= volumeSliderY_ - verticalPadding
        && y <= volumeSliderY_ + volumeSliderHeight_ + verticalPadding;
}

bool App::canSeekMediaProgress() const
{
    return hasCurrentSong_ && playing_ && currentSongDurationSeconds_ > 0.0f;
}

void App::refreshVolumeControl()
{
    const bool effectivelyMuted = volumeMuted_ || volume_ <= 0.0f;
    const float effectiveVolume = effectivelyMuted ? 0.0f : volume_;
    bool iconSourceChanged = false;

    if (Primitive* primitive = primitives_.find(volumeButtonId_)) {
        if (auto* button = std::get_if<ButtonPrimitive>(&primitive->geometry)) {
            const std::string& iconSvg = volumeIconFor(effectiveVolume, effectivelyMuted);
            if (button->iconSvg != iconSvg) {
                button->iconSvg = iconSvg;
                iconSourceChanged = true;
            }
            button->iconColor = effectivelyMuted ? rgb(133, 146, 137) : rgb(211, 198, 170);
        }
    }

    if (Primitive* primitive = primitives_.find(volumeSliderFillId_)) {
        if (auto* fill = std::get_if<RoundedRectPrimitive>(&primitive->geometry)) {
            fill->width = volumeSliderWidth_ * effectiveVolume;
            primitive->style.fill = effectivelyMuted ? rgb(133, 146, 137) : rgb(167, 192, 128);
            primitive->style.stroke = primitive->style.fill;
        }
    }

    if (Primitive* primitive = primitives_.find(volumeSliderKnobId_)) {
        if (auto* knob = std::get_if<CirclePrimitive>(&primitive->geometry)) {
            knob->centerX = volumeSliderX_ + volumeSliderWidth_ * effectiveVolume;
            primitive->style.fill = effectivelyMuted ? rgb(133, 146, 137) : rgb(211, 198, 170);
        }
    }

    refreshPrimitives(iconSourceChanged ? VulkanRenderer::PrimitiveUpdate::Svg : VulkanRenderer::PrimitiveUpdate::DrawOnly);
}

void App::refreshMediaProgressControl()
{
    const float progress = hasCurrentSong_ && currentSongDurationSeconds_ > 0.0f
        ? std::clamp(currentSongElapsedSeconds_ / currentSongDurationSeconds_, 0.0f, 1.0f)
        : 0.0f;
    bool textChanged = false;

    if (Primitive* primitive = primitives_.find(mediaProgressSliderFillId_)) {
        if (auto* fill = std::get_if<RoundedRectPrimitive>(&primitive->geometry)) {
            fill->width = mediaProgressSliderWidth_ * progress;
        }
    }

    if (Primitive* primitive = primitives_.find(mediaProgressSliderKnobId_)) {
        if (auto* knob = std::get_if<CirclePrimitive>(&primitive->geometry)) {
            knob->centerX = mediaProgressSliderX_ + mediaProgressSliderWidth_ * progress;
            primitive->style.fill = canSeekMediaProgress() ? rgb(211, 198, 170) : rgb(133, 146, 137);
        }
    }

    if (Primitive* primitive = primitives_.find(elapsedTimeTextId_)) {
        if (auto* text = std::get_if<TextPrimitive>(&primitive->geometry)) {
            const std::string timestamp = hasCurrentSong_ ? formatTimestamp(currentSongElapsedSeconds_) : "0:00";
            if (text->text != timestamp) {
                text->text = timestamp;
                textChanged = true;
            }
        }
    }

    if (Primitive* primitive = primitives_.find(totalTimeTextId_)) {
        if (auto* text = std::get_if<TextPrimitive>(&primitive->geometry)) {
            const std::string timestamp = hasCurrentSong_ ? formatTimestamp(currentSongDurationSeconds_) : "0:00";
            if (text->text != timestamp) {
                text->text = timestamp;
                textChanged = true;
            }
        }
    }

    refreshPrimitives(textChanged ? VulkanRenderer::PrimitiveUpdate::Text : VulkanRenderer::PrimitiveUpdate::DrawOnly);
}

void App::rebuildScene()
{
    primitives_.clear();
    audioScanStatusTextId_ = 0;
    audioScanPathTextId_ = 0;
    audioScanProgressFillId_ = 0;
    addSongsSearchFieldId_ = 0;
    buildInitialScene(static_cast<float>(window_.width()), static_cast<float>(window_.height()));
    pressedButton_ = 0;
    sceneReady_ = true;
    refreshPrimitives(VulkanRenderer::PrimitiveUpdate::Full);
}

void App::handlePointerEvent(const WaylandWindow::PointerEvent& event)
{
    if (addSongsAudioScanActive_) {
        window_.setCursor(WaylandWindow::CursorShape::Default);
        return;
    }

    Rect pendingListViewport;
    PendingSongsScrollbar pendingScrollbar;
    Rect pendingScrollbarHitTarget;
    if (addSongsMenuOpen_) {
        const Rect menu = addSongsMenuRect(
            static_cast<float>(window_.width()),
            static_cast<float>(window_.height()),
            addSongsDirectoryOptionsVisible_,
            addSongsPlaylistDropdownOpen_,
            playlists_.size());
        const Rect panel = addSongsFullListRect(
            static_cast<float>(window_.width()),
            static_cast<float>(window_.height()),
            menu,
            pendingAddSongs_.size());
        pendingListViewport = pendingSongsListViewportRect(panel);
        pendingScrollbar = pendingSongsScrollbar(pendingListViewport, pendingSongMatchCount(), pendingAddSongsScrollOffset_);
        if (pendingScrollbar.visible) {
            pendingScrollbarHitTarget = {
                .x = pendingScrollbar.track.x - (addSongsFullListScrollbarHitWidth - pendingScrollbar.track.width) * 0.5f,
                .y = pendingScrollbar.track.y,
                .width = addSongsFullListScrollbarHitWidth,
                .height = pendingScrollbar.track.height,
            };
        }
    }

    const auto setPendingScrollFromPointer = [&](float pointerY) {
        if (!pendingScrollbar.visible || pendingScrollbar.maxThumbTravel <= 0.0f) {
            return;
        }

        const float thumbY = std::clamp(
            pointerY - pendingSongsScrollbarDragOffsetY_,
            pendingScrollbar.track.y,
            pendingScrollbar.track.y + pendingScrollbar.maxThumbTravel);
        const float scrollRatio = (thumbY - pendingScrollbar.track.y) / pendingScrollbar.maxThumbTravel;
        const float rawOffset = pendingScrollbar.maxScrollOffset * scrollRatio;
        const float snappedOffset = std::round(rawOffset / addSongsFullListRowHeight) * addSongsFullListRowHeight;
        const float previousOffset = pendingAddSongsScrollOffset_;
        pendingAddSongsScrollOffset_ = std::clamp(snappedOffset, 0.0f, pendingScrollbar.maxScrollOffset);
        if (pendingAddSongsScrollOffset_ != previousOffset) {
            rebuildScene();
        }
    };

    if (draggingPendingSongsScrollbar_) {
        if (event.type == WaylandWindow::PointerEventType::Move) {
            setPendingScrollFromPointer(event.y);
            window_.setCursor(WaylandWindow::CursorShape::Pointer);
            return;
        }

        if (event.type == WaylandWindow::PointerEventType::ButtonRelease) {
            setPendingScrollFromPointer(event.y);
            draggingPendingSongsScrollbar_ = false;
            window_.setCursor(WaylandWindow::CursorShape::Pointer);
            return;
        }

        if (event.type == WaylandWindow::PointerEventType::Leave) {
            draggingPendingSongsScrollbar_ = false;
        }
    }

    const bool hoveringPendingSongsScrollbar = pendingScrollbar.visible
        && event.type != WaylandWindow::PointerEventType::Leave
        && contains(pendingScrollbarHitTarget, event.x, event.y);
    if (event.type == WaylandWindow::PointerEventType::ButtonPress && hoveringPendingSongsScrollbar) {
        draggingPendingSongsScrollbar_ = true;
        pendingSongsScrollbarDragOffsetY_ = contains(pendingScrollbar.thumb, event.x, event.y)
            ? event.y - pendingScrollbar.thumb.y
            : pendingScrollbar.thumb.height * 0.5f;
        setPendingScrollFromPointer(event.y);
        window_.setCursor(WaylandWindow::CursorShape::Pointer);
        return;
    }

    if (addSongsMenuOpen_ && event.type == WaylandWindow::PointerEventType::Scroll) {
        if (!contains(pendingListViewport, event.x, event.y) || event.scrollY == 0.0f) {
            return;
        }

        const float maxScrollOffset = pendingSongsMaxScrollOffset(pendingListViewport, pendingSongMatchCount());
        const float previousOffset = pendingAddSongsScrollOffset_;
        pendingAddSongsScrollOffset_ = std::clamp(
            pendingAddSongsScrollOffset_ + (event.scrollY > 0.0f ? addSongsFullListRowHeight : -addSongsFullListRowHeight),
            0.0f,
            maxScrollOffset);
        if (pendingAddSongsScrollOffset_ != previousOffset) {
            rebuildScene();
        }
        return;
    }

    if (addSongsMenuOpen_ && event.type == WaylandWindow::PointerEventType::ButtonPress && !addSongsMenuContains(event.x, event.y)) {
        closeAddSongsMenu();
        window_.setCursor(WaylandWindow::CursorShape::Default);
        return;
    }

    if (addSongsMenuOpen_ && event.type == WaylandWindow::PointerEventType::ButtonPress) {
        const bool searchFocused = addSongsSearchFieldContains(event.x, event.y);
        if (addSongsSearchFocused_ != searchFocused) {
            addSongsSearchFocused_ = searchFocused;
            resetAddSongsCaretBlink();
            rebuildScene();
        }
    }

    const bool hoveringMediaProgressSlider = !addSongsMenuOpen_
        && event.type != WaylandWindow::PointerEventType::Leave
        && canSeekMediaProgress()
        && mediaProgressSliderContains(event.x, event.y);
    const bool hoveringVolumeSlider = !addSongsMenuOpen_
        && event.type != WaylandWindow::PointerEventType::Leave
        && volumeSliderContains(event.x, event.y);

    if (event.type == WaylandWindow::PointerEventType::ButtonPress && hoveringMediaProgressSlider) {
        draggingMediaProgressSlider_ = true;
        setMediaProgressFromPointer(event.x);
        window_.setCursor(WaylandWindow::CursorShape::Pointer);
        return;
    }

    if (draggingMediaProgressSlider_) {
        if (event.type == WaylandWindow::PointerEventType::Move) {
            setMediaProgressFromPointer(event.x);
            window_.setCursor(WaylandWindow::CursorShape::Pointer);
            return;
        }

        if (event.type == WaylandWindow::PointerEventType::ButtonRelease) {
            setMediaProgressFromPointer(event.x);
            draggingMediaProgressSlider_ = false;
            window_.setCursor(hoveringMediaProgressSlider ? WaylandWindow::CursorShape::Pointer : WaylandWindow::CursorShape::Default);
            return;
        }

        if (event.type == WaylandWindow::PointerEventType::Leave) {
            draggingMediaProgressSlider_ = false;
        }
    }

    if (event.type == WaylandWindow::PointerEventType::ButtonPress && hoveringVolumeSlider) {
        draggingVolumeSlider_ = true;
        setVolumeFromPointer(event.x);
        window_.setCursor(WaylandWindow::CursorShape::Pointer);
        return;
    }

    if (draggingVolumeSlider_) {
        if (event.type == WaylandWindow::PointerEventType::Move) {
            setVolumeFromPointer(event.x);
            window_.setCursor(WaylandWindow::CursorShape::Pointer);
            return;
        }

        if (event.type == WaylandWindow::PointerEventType::ButtonRelease) {
            setVolumeFromPointer(event.x);
            draggingVolumeSlider_ = false;
            window_.setCursor(hoveringVolumeSlider ? WaylandWindow::CursorShape::Pointer : WaylandWindow::CursorShape::Default);
            return;
        }

        if (event.type == WaylandWindow::PointerEventType::Leave) {
            draggingVolumeSlider_ = false;
        }
    }

    bool changed = false;
    bool hasHoveredButton = false;
    PrimitiveId releasedButton = 0;

    for (Primitive& primitive : primitives_.all()) {
        auto* button = std::get_if<ButtonPrimitive>(&primitive.geometry);
        if (button == nullptr) {
            continue;
        }

        const bool isModalButton = addSongsMenuContains(
            button->x + button->width * 0.5f,
            button->y + button->height * 0.5f);
        const bool canInteract = primitive.visible
            && button->enabled
            && (!addSongsMenuOpen_ || isModalButton);
        const bool isHovered = canInteract
            && event.type != WaylandWindow::PointerEventType::Leave
            && contains(*button, event.x, event.y);

        if (button->hovered != isHovered) {
            button->hovered = isHovered;
            changed = true;
        }

        hasHoveredButton = hasHoveredButton || isHovered;

        if (event.type == WaylandWindow::PointerEventType::ButtonPress && isHovered) {
            pressedButton_ = primitive.id;
            if (!button->pressed) {
                button->pressed = true;
                changed = true;
            }
        } else if (event.type == WaylandWindow::PointerEventType::ButtonRelease || event.type == WaylandWindow::PointerEventType::Leave) {
            if (button->pressed) {
                button->pressed = false;
                changed = true;
            }

            if (event.type == WaylandWindow::PointerEventType::ButtonRelease && pressedButton_ == primitive.id && isHovered) {
                releasedButton = primitive.id;
            }
        }
    }

    if (event.type == WaylandWindow::PointerEventType::ButtonRelease || event.type == WaylandWindow::PointerEventType::Leave) {
        pressedButton_ = 0;
    }

    window_.setCursor(hasHoveredButton || hoveringPendingSongsScrollbar || hoveringMediaProgressSlider || hoveringVolumeSlider ? WaylandWindow::CursorShape::Pointer : WaylandWindow::CursorShape::Default);

    if (changed) {
        refreshPrimitives();
    }

    if (releasedButton != 0) {
        if (Primitive* primitive = primitives_.find(releasedButton)) {
            if (auto* button = std::get_if<ButtonPrimitive>(&primitive->geometry); button != nullptr && button->onClick) {
                button->onClick();

                bool hasPostClickHoveredButton = false;
                for (Primitive& postClickPrimitive : primitives_.all()) {
                    auto* postClickButton = std::get_if<ButtonPrimitive>(&postClickPrimitive.geometry);
                    if (postClickButton == nullptr) {
                        continue;
                    }

                    const bool isModalButton = addSongsMenuContains(
                        postClickButton->x + postClickButton->width * 0.5f,
                        postClickButton->y + postClickButton->height * 0.5f);
                    const bool canInteract = postClickPrimitive.visible
                        && postClickButton->enabled
                        && (!addSongsMenuOpen_ || isModalButton);
                    postClickButton->hovered = canInteract && contains(*postClickButton, event.x, event.y);
                    hasPostClickHoveredButton = hasPostClickHoveredButton || postClickButton->hovered;
                }

                window_.setCursor(hasPostClickHoveredButton || hoveringPendingSongsScrollbar || hoveringMediaProgressSlider || hoveringVolumeSlider ? WaylandWindow::CursorShape::Pointer : WaylandWindow::CursorShape::Default);
                refreshPrimitives();
            }
        }
    }
}

void App::handleKeyEvent(const WaylandWindow::KeyEvent& event)
{
    if (event.type != WaylandWindow::KeyEventType::Press) {
        return;
    }

    if (addSongsAudioScanActive_) {
        return;
    }

    if (event.key == KEY_ESC) {
        closeAddSongsMenu();
        return;
    }

    if (!addSongsSearchFocused_) {
        return;
    }

    resetAddSongsCaretBlink();

    if (event.key == KEY_BACKSPACE) {
        if (!addSongsSearchQuery_.empty()) {
            std::string query = addSongsSearchQuery_;
            query.pop_back();
            setAddSongsSearchQuery(std::move(query));
            rebuildScene();
        }
        return;
    }

    const char character = characterForKey(event.key);
    if (character != '\0') {
        std::string query = addSongsSearchQuery_;
        query.push_back(character);
        setAddSongsSearchQuery(std::move(query));
        rebuildScene();
    }
}

void App::refreshPrimitives(VulkanRenderer::PrimitiveUpdate update)
{
    pendingPrimitiveUpdate_ = combineUpdates(pendingPrimitiveUpdate_, update);
    primitivesDirty_ = true;
}

} // namespace womp
