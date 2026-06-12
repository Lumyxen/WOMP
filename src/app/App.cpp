#include "womp/App.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstddef>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <linux/input-event-codes.h>
#include <memory>
#include <mutex>
#include <span>
#include <sstream>
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
constexpr float createPlaylistMenuWidth = 480.0f;
constexpr float createPlaylistMenuHeight = 182.0f;
constexpr float minCreatePlaylistMenuWidth = 340.0f;
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
constexpr float sidebarPlaylistButtonHeight = 56.0f;
constexpr float sidebarPlaylistButtonGap = 8.0f;
constexpr float sidebarPlaylistRowStride = sidebarPlaylistButtonHeight + sidebarPlaylistButtonGap;
constexpr float sidebarPlaylistScrollbarWidth = 4.0f;
constexpr float sidebarPlaylistScrollbarHitWidth = 14.0f;
constexpr float sidebarPlaylistScrollbarMinThumbHeight = 28.0f;
constexpr float sidebarPlaylistScrollbarBottomInset = 8.0f;
constexpr float sidebarPlaylistEndPadding = 8.0f;
constexpr float playlistTrackRowHeight = 80.0f;
constexpr float playlistTrackRowGap = 8.0f;
constexpr float playlistTrackRowStride = playlistTrackRowHeight + playlistTrackRowGap;
constexpr float playlistTrackCoverSize = 64.0f;
constexpr float playlistTrackScrollbarWidth = 5.0f;
constexpr float playlistTrackScrollbarHitWidth = 14.0f;
constexpr float playlistTrackScrollbarInset = 2.0f;
constexpr float playlistSearchFieldHeight = 36.0f;
constexpr float playlistSearchFieldGap = 14.0f;
constexpr float playlistTrackTitleFontSize = 15.0f;
constexpr float playlistTrackTitleCharacterWidth = 7.8f;
constexpr float playlistTrackTitleYInset = 19.0f;
constexpr float playlistTrackFileTypeBadgeFontSize = 10.0f;
constexpr float playlistTrackFileTypeBadgeCharacterWidth = 5.5f;
constexpr float playlistTrackFileTypeBadgeHorizontalPadding = 4.0f;
constexpr float playlistTrackFileTypeBadgeGap = 8.0f;
constexpr float playlistTrackFileTypeBadgeHeight = 16.0f;
constexpr float playlistTrackFileTypeBadgeYInset = playlistTrackTitleYInset
    + (playlistTrackTitleFontSize * 1.25f - playlistTrackFileTypeBadgeHeight) * 0.5f;
constexpr float playlistTrackFileTypeBadgeTextYInset =
    (playlistTrackFileTypeBadgeHeight - playlistTrackFileTypeBadgeFontSize * 1.25f) * 0.5f;
constexpr float floatingMenuMargin = 16.0f;
constexpr char zenityPathSeparator = '\x1f';
constexpr std::chrono::milliseconds searchCaretBlinkInterval{500};
constexpr std::size_t playlistTitleCharacterLimit = 18;
constexpr std::size_t playlistDescriptionCharacterLimit = 120;
constexpr std::size_t playlistDescriptionLineLimit = 3;

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

Rect createPlaylistMenuRect(float windowWidth, float windowHeight)
{
    return centeredFitRect(
        windowWidth,
        windowHeight,
        createPlaylistMenuWidth,
        createPlaylistMenuHeight,
        minCreatePlaylistMenuWidth,
        createPlaylistMenuHeight,
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

std::string formatPlaylistCreationTime(std::chrono::system_clock::time_point createdAt)
{
    const std::time_t time = std::chrono::system_clock::to_time_t(createdAt);
    std::tm localTime{};
    if (localtime_r(&time, &localTime) == nullptr) {
        return "Unknown";
    }

    static constexpr std::array<std::string_view, 12> monthNames{
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };
    const int hour = localTime.tm_hour % 12 == 0 ? 12 : localTime.tm_hour % 12;

    std::ostringstream formatted;
    formatted << monthNames[localTime.tm_mon] << ' ' << localTime.tm_mday << ", "
              << localTime.tm_year + 1900 << ' ' << hour << ':';
    if (localTime.tm_min < 10) {
        formatted << '0';
    }
    formatted << localTime.tm_min << (localTime.tm_hour < 12 ? " AM" : " PM");
    return formatted.str();
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

char characterForKey(std::uint32_t key, bool shift)
{
    char letter = '\0';
    switch (key) {
    case KEY_A: letter = 'a'; break;
    case KEY_B: letter = 'b'; break;
    case KEY_C: letter = 'c'; break;
    case KEY_D: letter = 'd'; break;
    case KEY_E: letter = 'e'; break;
    case KEY_F: letter = 'f'; break;
    case KEY_G: letter = 'g'; break;
    case KEY_H: letter = 'h'; break;
    case KEY_I: letter = 'i'; break;
    case KEY_J: letter = 'j'; break;
    case KEY_K: letter = 'k'; break;
    case KEY_L: letter = 'l'; break;
    case KEY_M: letter = 'm'; break;
    case KEY_N: letter = 'n'; break;
    case KEY_O: letter = 'o'; break;
    case KEY_P: letter = 'p'; break;
    case KEY_Q: letter = 'q'; break;
    case KEY_R: letter = 'r'; break;
    case KEY_S: letter = 's'; break;
    case KEY_T: letter = 't'; break;
    case KEY_U: letter = 'u'; break;
    case KEY_V: letter = 'v'; break;
    case KEY_W: letter = 'w'; break;
    case KEY_X: letter = 'x'; break;
    case KEY_Y: letter = 'y'; break;
    case KEY_Z: letter = 'z'; break;
    default: break;
    }
    if (letter != '\0') {
        return shift ? static_cast<char>(letter - 'a' + 'A') : letter;
    }

    switch (key) {
    case KEY_0: return shift ? ')' : '0';
    case KEY_1: return shift ? '!' : '1';
    case KEY_2: return shift ? '@' : '2';
    case KEY_3: return shift ? '#' : '3';
    case KEY_4: return shift ? '$' : '4';
    case KEY_5: return shift ? '%' : '5';
    case KEY_6: return shift ? '^' : '6';
    case KEY_7: return shift ? '&' : '7';
    case KEY_8: return shift ? '*' : '8';
    case KEY_9: return shift ? '(' : '9';
    case KEY_SPACE: return ' ';
    case KEY_MINUS: return shift ? '_' : '-';
    case KEY_EQUAL: return shift ? '+' : '=';
    case KEY_LEFTBRACE: return shift ? '{' : '[';
    case KEY_RIGHTBRACE: return shift ? '}' : ']';
    case KEY_BACKSLASH: return shift ? '|' : '\\';
    case KEY_SEMICOLON: return shift ? ':' : ';';
    case KEY_APOSTROPHE: return shift ? '"' : '\'';
    case KEY_GRAVE: return shift ? '~' : '`';
    case KEY_COMMA: return shift ? '<' : ',';
    case KEY_DOT: return shift ? '>' : '.';
    case KEY_SLASH: return shift ? '?' : '/';
    default: break;
    }
    return '\0';
}

bool isWordCharacter(char character)
{
    const unsigned char value = static_cast<unsigned char>(character);
    return std::isalnum(value) != 0 || character == '_';
}

std::size_t previousWordBoundary(std::string_view text, std::size_t index)
{
    index = std::min(index, text.size());
    while (index > 0 && !isWordCharacter(text[index - 1])) {
        --index;
    }
    while (index > 0 && isWordCharacter(text[index - 1])) {
        --index;
    }
    return index;
}

std::size_t nextWordBoundary(std::string_view text, std::size_t index)
{
    index = std::min(index, text.size());
    while (index < text.size() && isWordCharacter(text[index])) {
        ++index;
    }
    while (index < text.size() && !isWordCharacter(text[index])) {
        ++index;
    }
    return index;
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

std::string joinArtists(const std::vector<std::string>& artists)
{
    std::string result;
    for (const std::string& artist : artists) {
        if (!result.empty()) {
            result += ", ";
        }
        result += artist;
    }
    return result.empty() ? "Unknown Artist" : result;
}

std::string formatDurationMs(std::int64_t durationMs)
{
    return formatTimestamp(static_cast<float>(durationMs) / 1000.0f);
}

std::string formatSummaryDuration(std::int64_t durationMs)
{
    const std::int64_t totalSeconds = std::max<std::int64_t>(0, durationMs) / 1000;
    const std::int64_t hours = totalSeconds / 3600;
    const std::int64_t minutes = (totalSeconds / 60) % 60;
    const std::int64_t seconds = totalSeconds % 60;
    char buffer[48]{};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%lld:%02lld:%02lld",
        static_cast<long long>(hours),
        static_cast<long long>(minutes),
        static_cast<long long>(seconds));
    return buffer;
}

std::string formatLastPlayed(std::optional<std::int64_t> timestampMs)
{
    if (!timestampMs) {
        return "Never";
    }
    return formatPlaylistCreationTime(
        std::chrono::system_clock::time_point{std::chrono::milliseconds{*timestampMs}});
}

std::int64_t currentTimeMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string importResultText(const ImportResult& result)
{
    return "Imported " + std::to_string(result.imported)
        + " | duplicate " + std::to_string(result.duplicates)
        + " | unsupported " + std::to_string(result.unsupported)
        + " | failed " + std::to_string(result.failed);
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

std::vector<std::filesystem::path> runPlaylistImportDialog(const std::filesystem::path& initialDirectory)
{
    const std::string command = std::string{"zenity --file-selection --multiple --separator=\"$(printf '\\037')\" "}
        + "--title='Import playlists' "
          "--file-filter='M3U playlists | *.m3u *.m3u8' "
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

std::filesystem::path runPlaylistExportDialog(
    const std::filesystem::path& initialDirectory,
    std::string playlistName)
{
    std::ranges::transform(playlistName, playlistName.begin(), [](unsigned char character) {
        return character == '/' || character == '\\' || std::iscntrl(character) != 0
            ? '_'
            : static_cast<char>(character);
    });
    if (playlistName.empty()) {
        playlistName = "playlist";
    }

    const std::filesystem::path suggestedPath = initialDirectory / (playlistName + ".m3u8");
    const std::string command = std::string{"zenity --file-selection --save --confirm-overwrite "}
        + "--title='Export playlist' "
          "--file-filter='M3U playlists | *.m3u *.m3u8' "
        + "--filename=" + shellQuote(suggestedPath.string())
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
    if (pclose(pipe) != 0) {
        return {};
    }
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
        output.pop_back();
    }
    if (output.empty()) {
        return {};
    }

    std::filesystem::path path{output};
    const std::string extension = lowercaseAscii(path.extension().string());
    if (extension != ".m3u" && extension != ".m3u8") {
        path += ".m3u8";
    }
    return path;
}

std::string displayNameForPath(const std::filesystem::path& path);

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

std::size_t maxTextCharacters(float width, float approximateCharacterWidth, std::size_t minimum = 1)
{
    return static_cast<std::size_t>(std::max(
        static_cast<float>(minimum),
        std::floor(std::max(0.0f, width) / approximateCharacterWidth)));
}

std::string fitTextToWidth(std::string text, float width, float approximateCharacterWidth)
{
    if (width < approximateCharacterWidth * 4.0f) {
        return {};
    }
    return truncateText(std::move(text), maxTextCharacters(width, approximateCharacterWidth, 4));
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

const std::string& playIconSvg()
{
    static const std::string icon = R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#ffffff" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round" class="lucide lucide-play-icon lucide-play"><path d="M5 5a2 2 0 0 1 3.008-1.728l11.997 6.998a2 2 0 0 1 .003 3.458l-12 7A2 2 0 0 1 5 19z"/></svg>)";
    return icon;
}

const std::string& pauseIconSvg()
{
    static const std::string icon = R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="lucide lucide-pause-icon lucide-pause"><rect x="14" y="3" width="5" height="18" rx="1"/><rect x="5" y="3" width="5" height="18" rx="1"/></svg>)";
    return icon;
}

} // namespace

void App::buildInitialScene(float windowWidth, float windowHeight)
{
    constexpr float preferredSidebarWidth = 280.0f;
    constexpr float minSidebarWidth = 180.0f;
    constexpr float sidebarPadding = 16.0f;
    constexpr float sidebarButtonHeight = 36.0f;
    constexpr float sidebarButtonGap = 4.0f;
    constexpr float sidebarPlaylistPinIconSize = 18.0f;
    constexpr float sidebarPlaylistPinIconInset = 12.0f;
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
    const std::string addPlaylistIcon = R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#ffffff" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="lucide lucide-list-plus-icon lucide-list-plus"><path d="M16 5H3"/><path d="M11 12H3"/><path d="M16 19H3"/><path d="M18 9v6"/><path d="M21 12h-6"/></svg>)";
    const std::string chevronUpIcon = R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#ffffff" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="lucide lucide-chevron-up-icon lucide-chevron-up"><path d="m18 15-6-6-6 6"/></svg>)";
    const std::string chevronDownIcon = R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#ffffff" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="lucide lucide-chevron-down-icon lucide-chevron-down"><path d="m6 9 6 6 6-6"/></svg>)";
    const std::string checkIcon = R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="lucide lucide-check-icon lucide-check"><path d="M20 6 9 17l-5-5"/></svg>)";
    const std::string closeIcon = R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="lucide lucide-x-icon lucide-x"><path d="M18 6 6 18"/><path d="m6 6 12 12"/></svg>)";
    const std::string settingsIcon = R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#859289" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="lucide lucide-settings2-icon lucide-settings-2"><path d="M14 17H5"/><path d="M19 7h-9"/><circle cx="17" cy="17" r="3"/><circle cx="7" cy="7" r="3"/></svg>)";
    const std::string playlistIcon = R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#859289" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="lucide lucide-list-music-icon lucide-list-music"><path d="M16 5H3"/><path d="M11 12H3"/><path d="M11 19H3"/><path d="M21 16V5"/><circle cx="18" cy="16" r="3"/></svg>)";
    const std::string pinIcon = R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="lucide lucide-pin-icon lucide-pin"><path d="M12 17v5"/><path d="M9 10.76a2 2 0 0 1-1.11 1.79l-1.78.9A2 2 0 0 0 5 15.24V16a1 1 0 0 0 1 1h12a1 1 0 0 0 1-1v-.76a2 2 0 0 0-1.11-1.79l-1.78-.9A2 2 0 0 1 15 10.76V7a1 1 0 0 1 1-1 2 2 0 0 0 0-4H8a2 2 0 0 0 0 4 1 1 0 0 1 1 1z"/></svg>)";
    const std::string pinOffIcon = R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#ffffff" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="lucide lucide-pin-off-icon lucide-pin-off"><path d="M12 17v5"/><path d="M15 9.34V7a1 1 0 0 1 1-1 2 2 0 0 0 0-4H7.89"/><path d="m2 2 20 20"/><path d="M9 9v1.76a2 2 0 0 1-1.11 1.79l-1.78.9A2 2 0 0 0 5 15.24V16a1 1 0 0 0 1 1h11"/></svg>)";
    const std::string exportIcon = R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#ffffff" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="lucide lucide-upload-icon lucide-upload"><path d="M12 3v12"/><path d="m17 8-5-5-5 5"/><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/></svg>)";
    const std::string deleteIcon = R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#ffffff" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="lucide lucide-trash2-icon lucide-trash-2"><path d="M10 11v6"/><path d="M14 11v6"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6"/><path d="M3 6h18"/><path d="M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/></svg>)";
    const std::string shuffleIcon = R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#ffffff" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="lucide lucide-shuffle-icon lucide-shuffle"><path d="m18 14 4 4-4 4"/><path d="m18 2 4 4-4 4"/><path d="M2 18h1.973a4 4 0 0 0 3.3-1.7l5.454-8.6a4 4 0 0 1 3.3-1.7H22"/><path d="M2 6h1.972a4 4 0 0 1 3.6 2.2"/><path d="M22 18h-6.041a4 4 0 0 1-3.3-1.8l-.359-.45"/></svg>)";
    const std::string defaultCoverIcon = R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#ffffff" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="lucide lucide-image-icon lucide-image"><rect width="18" height="18" x="3" y="3" rx="2" ry="2"/><circle cx="9" cy="9" r="2"/><path d="m21 15-3.086-3.086a2 2 0 0 0-2.828 0L6 21"/></svg>)";
    const std::string ellipsisIcon = R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#ffffff" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="lucide lucide-ellipsis-icon lucide-ellipsis"><circle cx="12" cy="12" r="1"/><circle cx="19" cy="12" r="1"/><circle cx="5" cy="12" r="1"/></svg>)";
    const std::string backwardIcon = R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="lucide lucide-skip-back-icon lucide-skip-back"><path d="M17.971 4.285A2 2 0 0 1 21 6v12a2 2 0 0 1-3.029 1.715l-9.997-5.998a2 2 0 0 1-.003-3.432z"/><path d="M3 20V4"/></svg>)";
    const std::string& pauseIcon = pauseIconSvg();
    const std::string& playIcon = playIconSvg();
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

    const auto addPlaylistButton = [&](PlaylistId id, std::string label, std::string iconSvg, float y, float width) {
        const bool selected = selectedPlaylistId_ == id;
        return primitives_.add(Primitive::button(
            {
                .x = sidebarPadding,
                .y = y,
                .width = width,
                .height = sidebarPlaylistButtonHeight,
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

    const auto addHeaderActionButton = [&](const std::string& iconSvg, float x, float y, float size, bool primary = false, bool danger = false, std::function<void()> onClick = {}) {
        const Color dangerColor = rgb(224, 91, 91);
        const Color dangerHover = rgb(91, 48, 51);
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
                .iconColor = primary ? sidebarBackground : (danger ? dangerColor : sidebarText),
                .hoverFill = primary ? sidebarText : (danger ? dangerHover : mantle),
                .pressedFill = primary ? iconGrey : (danger ? dangerHover : activeFill),
                .hoverStroke = primary ? sidebarText : (danger ? dangerColor : border),
                .pressedStroke = danger ? dangerColor : accent,
                .onClick = std::move(onClick),
            },
            {
                .fill = primary ? accent : transparent,
                .stroke = primary ? accent : (danger ? dangerColor : border),
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
    volumeSliderHitHeight_ = volumeSliderKnobRadius * 2.0f;
    mediaProgressSliderX_ = transportBarX;
    mediaProgressSliderY_ = transportBarY;
    mediaProgressSliderWidth_ = transportBarWidth;
    mediaProgressSliderHeight_ = transportBarThickness;
    mediaProgressSliderHitHeight_ = transportBarKnobRadius * 2.0f;
    addShuffleButton(transportX - shuffleButtonSize - transportButtonGap, transportY + (transportButtonSize - shuffleButtonSize) * 0.5f);
    addTransportButton(backwardIcon, transportX, transportY, [this] { playPrevious(); });
    playPauseButtonId_ = addTransportButton(playing_ ? pauseIcon : playIcon, transportX + transportButtonSize + transportButtonGap, transportY, [this]() {
        togglePlayback();
    });
    addTransportButton(forwardIcon, transportX + (transportButtonSize + transportButtonGap) * 2.0f, transportY, [this] { playNext(true); });
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
    const float contentBottom = windowHeight - bottomBarHeight;
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
    constexpr float maximumHeaderDescriptionWidth = 360.0f;
    constexpr float headerIconTileSize = 116.0f;
    constexpr float headerStatsTop = headerPadding + headerIconTileSize + 12.0f;
    constexpr float headerStatHeight = 48.0f;
    constexpr float headerStatGap = 8.0f;
    const bool allSongsSelected = selectedPlaylist == nullptr;
    const std::size_t playlistSongCount = allSongsSelected ? tracks_.size() : selectedPlaylist->trackIndexes.size();
    const std::string groupedSongCount = formatGroupedNumber(playlistSongCount, numberGroupingSeparator_);
    const std::string playlistDescription = allSongsSelected
        ? "Every song in your library."
        : selectedPlaylist->description;
    const PlaylistSummary selectedSummary = libraryStore_.summary(
        allSongsSelected ? std::nullopt : std::optional<PlaylistId>{selectedPlaylistId_});
    selectedPersistedListenedMs_ = selectedSummary.listenedMs;
    const std::optional<PlaylistId> selectedPlaylistScope = allSongsSelected
        ? std::nullopt
        : std::optional<PlaylistId>{selectedPlaylistId_};
    std::vector<std::pair<std::string_view, std::string>> playlistStats{
        {"SONGS", groupedSongCount},
        {"TOTAL TIME", formatSummaryDuration(selectedSummary.totalDurationMs)},
        {"LISTENED", formatSummaryDuration(
            selectedPersistedListenedMs_ + playbackStats_.pendingListenedMsFor(selectedPlaylistScope))},
        {"LAST PLAYED", formatLastPlayed(selectedSummary.lastPlayedAtMs)},
    };
    if (!allSongsSelected) {
        playlistStats.emplace_back("TIME CREATED", formatPlaylistCreationTime(selectedPlaylist->createdAt));
    }
    const std::size_t headerActionCount = allSongsSelected ? 1 : 4;
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
    const std::size_t headerStatColumnCount = headerStatsWidth >= 360.0f ? playlistStats.size() : 2;
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
    const float headerDescriptionWidth = allSongsSelected
        ? headerTextWidth
        : std::min(
              maximumHeaderDescriptionWidth,
              std::max(0.0f, headerActionsX - headerTextX - headerGap));

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
    if (allSongsSelected) {
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
    } else {
        const bool titleFocused = playlistMetadataField_ == TextFieldTarget::PlaylistTitle;
        const bool descriptionFocused = playlistMetadataField_ == TextFieldTarget::PlaylistDescription;
        const std::string& titleText = titleFocused ? playlistTitleDraft_ : selectedPlaylist->name;
        const std::string& descriptionText = descriptionFocused
            ? playlistDescriptionDraft_
            : selectedPlaylist->description;
        clampTextEditState(playlistTitleEdit_, titleText);
        clampTextEditState(playlistDescriptionEdit_, descriptionText);

        playlistTitleFieldId_ = primitives_.add(Primitive::textField(
            {
                .x = headerTextX,
                .y = contentY + 42.0f,
                .width = headerTextWidth,
                .height = 32.0f,
                .radius = 0.0f,
                .padding = 0.0f,
                .fontSize = 25.0f,
                .caretWidth = 1.0f,
                .text = titleText,
                .textColor = sidebarText,
                .caretColor = sidebarText,
                .selectionColor = accent,
                .selectedTextColor = mantle,
                .caretCodepointIndex = playlistTitleEdit_.caretIndex,
                .selectionStartCodepointIndex = std::min(playlistTitleEdit_.caretIndex, playlistTitleEdit_.selectionAnchor),
                .selectionEndCodepointIndex = std::max(playlistTitleEdit_.caretIndex, playlistTitleEdit_.selectionAnchor),
                .focused = titleFocused,
                .caretVisible = searchCaretVisible_,
            },
            {
                .fill = transparent,
                .stroke = transparent,
                .strokeWidth = 0.0f,
            }));
        playlistDescriptionFieldId_ = primitives_.add(Primitive::textField(
            {
                .x = headerTextX,
                .y = contentY + 78.0f,
                .width = headerDescriptionWidth,
                .height = 54.0f,
                .radius = 0.0f,
                .padding = 0.0f,
                .fontSize = 14.0f,
                .caretWidth = 1.0f,
                .text = descriptionText,
                .placeholder = "No description for this playlist.",
                .textColor = iconGrey,
                .placeholderColor = iconGrey,
                .caretColor = iconGrey,
                .selectionColor = accent,
                .selectedTextColor = mantle,
                .caretCodepointIndex = playlistDescriptionEdit_.caretIndex,
                .selectionStartCodepointIndex = std::min(playlistDescriptionEdit_.caretIndex, playlistDescriptionEdit_.selectionAnchor),
                .selectionEndCodepointIndex = std::max(playlistDescriptionEdit_.caretIndex, playlistDescriptionEdit_.selectionAnchor),
                .wrapMode = TextWrapMode::Word,
                .maxVisibleLines = playlistDescriptionLineLimit,
                .focused = descriptionFocused,
                .caretVisible = searchCaretVisible_,
            },
            {
                .fill = transparent,
                .stroke = transparent,
                .strokeWidth = 0.0f,
            }));
    }
    float headerActionX = headerActionsX;
    const auto addHeaderAction = [&](const std::string& icon, bool primary = false, bool danger = false, std::function<void()> onClick = {}) {
        addHeaderActionButton(icon, headerActionX, headerActionsY, headerActionSize, primary, danger, std::move(onClick));
        headerActionX += headerActionSize + headerActionGap;
    };
    if (!allSongsSelected) {
        addHeaderAction(deleteIcon, false, true, [this, playlistId = selectedPlaylistId_]() {
            if (confirmPlaylistDeletion(playlistId)) {
                removePlaylist(playlistId);
            }
        });
        addHeaderAction(selectedPlaylist->pinned ? pinOffIcon : pinIcon, selectedPlaylist->pinned, false, [this, playlistId = selectedPlaylistId_]() {
            const auto playlist = std::ranges::find(playlists_, playlistId, &Playlist::id);
            if (playlist != playlists_.end()) {
                setPlaylistPinned(playlistId, !playlist->pinned);
            }
        });
        addHeaderAction(exportIcon, false, false, [this, playlistId = selectedPlaylistId_]() {
            exportPlaylist(playlistId);
        });
    }
    addHeaderAction(selectedSourceIsCurrent() && playing_ ? pauseIcon : playIcon, true, false, [this] {
        if (selectedSourceIsCurrent()) {
            togglePlayback();
        } else {
            playSelectedSource();
        }
    });

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
        const PrimitiveId statValueId = primitives_.add(Primitive::text(
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
        if (statIndex == 2) {
            listenedTimeTextId_ = statValueId;
            listenedTimeMaxCharacters_ = maxStatValueCharacters;
        }
    }

    const float searchY = contentY + headerHeight + 18.0f;
    clampTextEditState(playlistSearchEdit_, playlistSearchQuery_);
    const float playlistSearchScrollOffset = textFieldScrollOffset(
        playlistSearchEdit_,
        playlistSearchQuery_,
        contentWidth - 20.0f,
        14.0f);
    playlistSearchFieldId_ = primitives_.add(Primitive::textField(
        {
            .x = contentX,
            .y = searchY,
            .width = contentWidth,
            .height = playlistSearchFieldHeight,
            .radius = 0.0f,
            .padding = 10.0f,
            .fontSize = 14.0f,
            .caretWidth = 1.0f,
            .text = playlistSearchQuery_,
            .placeholder = "Search songs in playlist",
            .textColor = sidebarText,
            .placeholderColor = iconGrey,
            .caretColor = sidebarText,
            .selectionColor = accent,
            .selectedTextColor = mantle,
            .caretCodepointIndex = playlistSearchEdit_.caretIndex,
            .selectionStartCodepointIndex = std::min(playlistSearchEdit_.caretIndex, playlistSearchEdit_.selectionAnchor),
            .selectionEndCodepointIndex = std::max(playlistSearchEdit_.caretIndex, playlistSearchEdit_.selectionAnchor),
            .horizontalScrollOffset = playlistSearchScrollOffset,
            .focused = playlistSearchFocused_,
            .caretVisible = searchCaretVisible_,
        },
        {
            .fill = mantle,
            .stroke = playlistSearchFocused_ ? accent : border,
            .strokeWidth = 1.0f,
        }));

    playlistTrackRows_.clear();
    hoveredPlaylistTrackSlot_ = static_cast<std::size_t>(-1);
    const float trackY = searchY + playlistSearchFieldHeight + playlistSearchFieldGap;
    const float availableTrackHeight = std::max(0.0f, contentBottom - trackY);
    playlistTrackViewportX_ = contentX;
    playlistTrackViewportY_ = trackY;
    playlistTrackViewportWidth_ = std::max(0.0f, contentWidth - playlistTrackScrollbarHitWidth);
    playlistTrackViewportHeight_ = availableTrackHeight;
    const std::size_t visibleTrackCapacity = static_cast<std::size_t>(
        std::ceil(playlistTrackViewportHeight_ / playlistTrackRowStride));
    const PrimitiveClipRect trackViewportClip{
        .x = playlistTrackViewportX_,
        .y = playlistTrackViewportY_,
        .width = playlistTrackViewportWidth_,
        .height = playlistTrackViewportHeight_,
    };

    constexpr float rowHorizontalPadding = 8.0f;
    constexpr float coverGap = 14.0f;
    constexpr float actionButtonSize = 32.0f;
    constexpr float actionButtonGap = 6.0f;
    constexpr float durationWidth = 42.0f;
    constexpr float rightPadding = 12.0f;
    const float rowContentRight = contentX + playlistTrackViewportWidth_ - rightPadding;
    const float ellipsisX = rowContentRight - actionButtonSize;
    const float playX = ellipsisX - actionButtonGap - actionButtonSize;
    const float durationX = rowContentRight - durationWidth;
    const float hoveredDurationX = playX - 12.0f - durationWidth;
    const float titleX = contentX + rowHorizontalPadding + playlistTrackCoverSize + coverGap;
    const float artistX = std::max(titleX + 72.0f, contentX + contentWidth * 0.62f);
    const float titleRight = std::max(titleX, artistX - 20.0f);
    const Color coverFill = rgb(52, 63, 68);
    const Color buttonHoverFill = rgb(71, 82, 88);
    const Color buttonPressedFill = rgb(63, 74, 69);

    const auto addRowText = [&](float x, float y, float fontSize, std::string text, Color color) {
        return primitives_.add(Primitive::text(
            {.x = x, .y = y, .fontSize = fontSize, .text = std::move(text)},
            {.fill = color, .stroke = color, .strokeWidth = 0.0f}));
    };
    const auto addRowActionButton = [&](float x, float y, const std::string& icon) {
        return primitives_.add(Primitive::button(
            {
                .x = x,
                .y = y,
                .width = actionButtonSize,
                .height = actionButtonSize,
                .padding = 7.0f,
                .radius = 0.0f,
                .iconSize = 18.0f,
                .iconSvg = icon,
                .iconColor = transparent,
                .hoverFill = buttonHoverFill,
                .pressedFill = buttonPressedFill,
                .disabledFill = transparent,
                .hoverStroke = transparent,
                .pressedStroke = transparent,
                .enabled = false,
            },
            {.fill = transparent, .stroke = transparent, .strokeWidth = 0.0f}));
    };

    for (std::size_t slotIndex = 0; slotIndex < visibleTrackCapacity; ++slotIndex) {
        const float rowY = trackY + static_cast<float>(slotIndex) * playlistTrackRowStride;
        PlaylistTrackRowPrimitives row;
        row.background = primitives_.add(Primitive::roundedRect(
            {.x = contentX, .y = rowY, .width = playlistTrackViewportWidth_, .height = playlistTrackRowHeight, .radius = 0.0f},
            {.fill = transparent, .stroke = transparent, .strokeWidth = 0.0f}));
        row.coverTile = primitives_.add(Primitive::roundedRect(
            {
                .x = contentX + rowHorizontalPadding,
                .y = rowY + 8.0f,
                .width = playlistTrackCoverSize,
                .height = playlistTrackCoverSize,
                .radius = 0.0f,
            },
            {.fill = coverFill, .stroke = border, .strokeWidth = 1.0f}));
        row.coverIcon = primitives_.add(Primitive::svg(
            {
                .x = contentX + rowHorizontalPadding + 20.0f,
                .y = rowY + 28.0f,
                .width = 24.0f,
                .height = 24.0f,
                .rasterScale = 4.0f,
                .source = defaultCoverIcon,
                .sourceType = SvgSourceType::Data,
                .renderMode = SvgRenderMode::Mask,
            },
            {.fill = iconGrey, .stroke = transparent, .strokeWidth = 0.0f}));
        row.fileTypeBadgeBackground = primitives_.add(Primitive::roundedRect(
            {
                .x = titleRight,
                .y = rowY + playlistTrackFileTypeBadgeYInset,
                .width = 0.0f,
                .height = playlistTrackFileTypeBadgeHeight,
                .radius = 0.0f,
            },
            {.fill = transparent, .stroke = transparent, .strokeWidth = 0.0f}));
        row.title = addRowText(
            titleX,
            rowY + playlistTrackTitleYInset,
            playlistTrackTitleFontSize,
            "",
            sidebarText);
        row.fileTypeBadge = addRowText(
            titleRight,
            rowY + playlistTrackFileTypeBadgeYInset + playlistTrackFileTypeBadgeTextYInset,
            playlistTrackFileTypeBadgeFontSize,
            "",
            accent);
        row.album = addRowText(titleX, rowY + 45.0f, 13.0f, "", iconGrey);
        row.artist = addRowText(artistX, rowY + 31.0f, 14.0f, "", iconGrey);
        row.duration = addRowText(durationX, rowY + 31.0f, 13.0f, "", sidebarText);
        row.hoveredDuration = addRowText(hoveredDurationX, rowY + 31.0f, 13.0f, "", transparent);
        row.playButton = addRowActionButton(playX, rowY + 24.0f, playIcon);
        row.ellipsisButton = addRowActionButton(ellipsisX, rowY + 24.0f, ellipsisIcon);
        for (const PrimitiveId id : {
                 row.background,
                 row.coverTile,
                 row.coverIcon,
                 row.title,
                 row.fileTypeBadgeBackground,
                 row.fileTypeBadge,
                 row.album,
                 row.artist,
                 row.duration,
                 row.hoveredDuration,
                 row.playButton,
                 row.ellipsisButton,
             }) {
            if (Primitive* primitive = primitives_.find(id)) {
                primitive->clip = trackViewportClip;
            }
        }
        playlistTrackRows_.push_back(row);
    }
    playlistFirstVisibleRow_ = std::min(playlistFirstVisibleRow_, playlistTrackMaxFirstVisibleRow());

    playlistScrollbarTrackX_ = contentX + contentWidth - playlistTrackScrollbarInset - playlistTrackScrollbarWidth;
    playlistScrollbarTrackY_ = trackY + playlistTrackScrollbarInset;
    playlistScrollbarTrackWidth_ = playlistTrackScrollbarWidth;
    playlistScrollbarTrackHeight_ = std::max(0.0f, playlistTrackViewportHeight_ - playlistTrackScrollbarInset * 2.0f);
    playlistScrollbarTrackId_ = primitives_.add(Primitive::roundedRect(
        {
            .x = playlistScrollbarTrackX_,
            .y = playlistScrollbarTrackY_,
            .width = playlistScrollbarTrackWidth_,
            .height = playlistScrollbarTrackHeight_,
            .radius = 0.0f,
        },
        {.fill = transparent, .stroke = transparent, .strokeWidth = 0.0f}));
    playlistScrollbarThumbId_ = primitives_.add(Primitive::roundedRect(
        {
            .x = playlistScrollbarTrackX_,
            .y = playlistScrollbarTrackY_,
            .width = playlistScrollbarTrackWidth_,
            .height = 0.0f,
            .radius = 0.0f,
        },
        {.fill = iconGrey, .stroke = transparent, .strokeWidth = 0.0f}));
    refreshPlaylistTrackRows();

    if (displayedPlaylistTrackCount() == 0) {
        primitives_.add(Primitive::text(
            {
                .x = contentX,
                .y = trackY + 8.0f,
                .fontSize = 15.0f,
                .text = normalizedPlaylistSearchQuery_.empty()
                    ? "No songs in this playlist"
                    : "No songs match your search",
            },
            {
                .fill = iconGrey,
                .stroke = iconGrey,
                .strokeWidth = 0.0f,
            }));
    }

    addPlaylistButton(0, "All Songs", playlistIcon, sidebarPadding, sidebarButtonWidth);
    primitives_.add(Primitive::line(
        {
            .x0 = sidebarPadding,
            .y0 = sidebarPadding + sidebarPlaylistButtonHeight + sidebarPadding,
            .x1 = sidebarWidth - sidebarPadding,
            .y1 = sidebarPadding + sidebarPlaylistButtonHeight + sidebarPadding,
            .thickness = 1.0f,
        },
        {
            .fill = separator,
            .stroke = separator,
            .strokeWidth = 0.0f,
        }));

    const float helpY = windowHeight - sidebarPadding - sidebarButtonHeight;
    const float addSongsY = helpY - sidebarButtonGap - sidebarButtonHeight;
    const float createPlaylistY = addSongsY - sidebarButtonGap - sidebarButtonHeight;
    const float bottomActionsSeparatorY = createPlaylistY - sidebarPadding;
    const float playlistY = sidebarPadding + sidebarPlaylistButtonHeight + sidebarPadding + sidebarPlaylistButtonGap;
    sidebarPlaylistViewportX_ = sidebarPadding;
    sidebarPlaylistViewportY_ = playlistY;
    sidebarPlaylistViewportWidth_ = sidebarButtonWidth;
    sidebarPlaylistViewportHeight_ = std::max(0.0f, bottomActionsSeparatorY - playlistY);
    sidebarPlaylistFirstVisibleRow_ = std::min(
        sidebarPlaylistFirstVisibleRow_,
        sidebarPlaylistMaxFirstVisibleRow());

    const std::size_t maxFirstVisiblePlaylist = sidebarPlaylistMaxFirstVisibleRow();
    const bool sidebarPlaylistScrollbarVisible = maxFirstVisiblePlaylist > 0
        && sidebarPlaylistViewportHeight_ > 0.0f;
    const float playlistButtonWidth = sidebarPlaylistScrollbarVisible
        ? std::max(0.0f, sidebarButtonWidth - sidebarPlaylistScrollbarHitWidth)
        : sidebarButtonWidth;
    const float playlistContentHeight = playlists_.empty()
        ? 0.0f
        : static_cast<float>(playlists_.size()) * sidebarPlaylistRowStride
            - sidebarPlaylistButtonGap
            + sidebarPlaylistEndPadding;
    const float maxPlaylistScrollOffset = std::max(
        0.0f,
        playlistContentHeight - sidebarPlaylistViewportHeight_);
    const bool atPlaylistScrollEnd = sidebarPlaylistFirstVisibleRow_ == maxFirstVisiblePlaylist
        && maxFirstVisiblePlaylist > 0;
    const float playlistScrollOffset = atPlaylistScrollEnd
        ? maxPlaylistScrollOffset
        : std::min(
            static_cast<float>(sidebarPlaylistFirstVisibleRow_) * sidebarPlaylistRowStride,
            maxPlaylistScrollOffset);
    const std::size_t firstRenderedPlaylist = std::min(
        playlists_.size(),
        static_cast<std::size_t>(playlistScrollOffset / sidebarPlaylistRowStride));
    const float firstPlaylistY = playlistY - std::fmod(playlistScrollOffset, sidebarPlaylistRowStride);
    for (std::size_t playlistIndex = firstRenderedPlaylist; playlistIndex < playlists_.size(); ++playlistIndex) {
        const float rowY = firstPlaylistY
            + static_cast<float>(playlistIndex - firstRenderedPlaylist) * sidebarPlaylistRowStride;
        if (rowY >= playlistY + sidebarPlaylistViewportHeight_) {
            break;
        }

        const Playlist& playlist = playlists_[playlistIndex];
        const PrimitiveId playlistButtonId = addPlaylistButton(
            playlist.id,
            playlist.name,
            playlistIcon,
            rowY,
            playlistButtonWidth);
        if (Primitive* primitive = primitives_.find(playlistButtonId)) {
            primitive->clip = PrimitiveClipRect{
                .x = sidebarPlaylistViewportX_,
                .y = sidebarPlaylistViewportY_,
                .width = sidebarPlaylistViewportWidth_,
                .height = sidebarPlaylistViewportHeight_,
            };
        }
        if (playlist.pinned) {
            Primitive pin = Primitive::svg(
                {
                    .x = sidebarPadding + playlistButtonWidth - sidebarPlaylistPinIconInset - sidebarPlaylistPinIconSize,
                    .y = rowY + (sidebarPlaylistButtonHeight - sidebarPlaylistPinIconSize) * 0.5f,
                    .width = sidebarPlaylistPinIconSize,
                    .height = sidebarPlaylistPinIconSize,
                    .rasterScale = 4.0f,
                    .source = pinIcon,
                    .sourceType = SvgSourceType::Data,
                    .renderMode = SvgRenderMode::Mask,
                },
                {
                    .fill = accent,
                    .stroke = transparent,
                    .strokeWidth = 0.0f,
                });
            pin.clip = PrimitiveClipRect{
                .x = sidebarPlaylistViewportX_,
                .y = sidebarPlaylistViewportY_,
                .width = sidebarPlaylistViewportWidth_,
                .height = sidebarPlaylistViewportHeight_,
            };
            primitives_.add(std::move(pin));
        }
    }

    sidebarPlaylistScrollbarTrackX_ = sidebarWidth - sidebarPadding - sidebarPlaylistScrollbarWidth;
    sidebarPlaylistScrollbarTrackY_ = playlistY;
    sidebarPlaylistScrollbarTrackWidth_ = sidebarPlaylistScrollbarWidth;
    sidebarPlaylistScrollbarTrackHeight_ = std::max(
        0.0f,
        sidebarPlaylistViewportHeight_ - sidebarPlaylistScrollbarBottomInset);
    sidebarPlaylistScrollbarThumbY_ = playlistY;
    sidebarPlaylistScrollbarThumbHeight_ = 0.0f;
    if (sidebarPlaylistScrollbarVisible) {
        const float visibleRatio = playlistContentHeight <= 0.0f
            ? 1.0f
            : sidebarPlaylistViewportHeight_ / playlistContentHeight;
        sidebarPlaylistScrollbarThumbHeight_ = std::min(
            sidebarPlaylistScrollbarTrackHeight_,
            std::max(
                sidebarPlaylistScrollbarMinThumbHeight,
                sidebarPlaylistScrollbarTrackHeight_ * visibleRatio));
        const float thumbTravel = std::max(
            0.0f,
            sidebarPlaylistScrollbarTrackHeight_ - sidebarPlaylistScrollbarThumbHeight_);
        sidebarPlaylistScrollbarThumbY_ = sidebarPlaylistScrollbarTrackY_
            + thumbTravel
                * playlistScrollOffset
                / maxPlaylistScrollOffset;

        primitives_.add(Primitive::roundedRect(
            {
                .x = sidebarPlaylistScrollbarTrackX_,
                .y = sidebarPlaylistScrollbarTrackY_,
                .width = sidebarPlaylistScrollbarTrackWidth_,
                .height = sidebarPlaylistScrollbarTrackHeight_,
                .radius = 0.0f,
            },
            {.fill = mantle, .stroke = transparent, .strokeWidth = 0.0f}));
        primitives_.add(Primitive::roundedRect(
            {
                .x = sidebarPlaylistScrollbarTrackX_,
                .y = sidebarPlaylistScrollbarThumbY_,
                .width = sidebarPlaylistScrollbarTrackWidth_,
                .height = sidebarPlaylistScrollbarThumbHeight_,
                .radius = 0.0f,
            },
            {.fill = iconGrey, .stroke = transparent, .strokeWidth = 0.0f}));
    }

    primitives_.add(Primitive::line(
        {
            .x0 = sidebarPadding,
            .y0 = bottomActionsSeparatorY,
            .x1 = sidebarWidth - sidebarPadding,
            .y1 = bottomActionsSeparatorY,
            .thickness = 1.0f,
        },
        {
            .fill = separator,
            .stroke = separator,
            .strokeWidth = 0.0f,
        }));
    addSidebarButton("Create Playlist", addPlaylistIcon, createPlaylistY, [this]() {
        toggleCreatePlaylistMenu();
    });
    addSidebarButton("Import Songs", addSongsIcon, addSongsY, [this]() {
        toggleAddSongsMenu();
    });
    addSidebarButton("Settings", settingsIcon, helpY);

    if (createPlaylistMenuOpen_) {
        constexpr float menuPadding = 20.0f;
        constexpr float menuButtonHeight = 44.0f;
        constexpr float menuButtonGap = 10.0f;
        constexpr float closeButtonSize = 30.0f;
        constexpr float closeIconSize = 18.0f;
        const Rect menu = createPlaylistMenuRect(windowWidth, windowHeight);
        const Color menuBackground = rgb(52, 63, 68);
        const Color dimOverlay = rgb(30, 35, 38, 0.72f);
        const Color primaryButtonFill = rgb(55, 65, 69);
        const Color primaryButtonHoverFill = rgb(65, 75, 80);
        const Color primaryButtonPressedFill = rgb(73, 81, 86);
        const auto addMenuButton = [&](std::string label, float y, std::function<void()> onClick) {
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
                    .hoverLabelColor = sidebarText,
                    .pressedLabelColor = sidebarText,
                    .hoverFill = primaryButtonHoverFill,
                    .pressedFill = primaryButtonPressedFill,
                    .hoverStroke = accent,
                    .pressedStroke = accent,
                    .onClick = std::move(onClick),
                    .centerLabel = false,
                },
                {
                    .fill = primaryButtonFill,
                    .stroke = accent,
                    .strokeWidth = 1.0f,
                }));
        };

        firstModalPrimitiveId_ = primitives_.add(Primitive::quad(
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
                .text = "Create Playlist",
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
                    closeCreatePlaylistMenu();
                },
            },
            {
                .fill = transparent,
                .stroke = transparent,
                .strokeWidth = 1.0f,
            }));

        float menuButtonY = menu.y + 64.0f;
        addMenuButton("Import Playlist", menuButtonY, [this]() {
            beginImportPlaylistFiles();
        });
        menuButtonY += menuButtonHeight + menuButtonGap;
        addMenuButton("Create New Playlist", menuButtonY, [this]() {
            createPlaylistMenuOpen_ = false;
            createPlaylist();
        });
    }

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

        firstModalPrimitiveId_ = primitives_.add(Primitive::quad(
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
        if (!lastImportResultText_.empty()) {
            primitives_.add(Primitive::text(
                {
                    .x = menu.x + 170.0f,
                    .y = menu.y + 29.0f,
                    .fontSize = 11.0f,
                    .text = truncateText(lastImportResultText_, 44),
                },
                {
                    .fill = menuSubtleText,
                    .stroke = menuSubtleText,
                    .strokeWidth = 0.0f,
                }));
        }
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
                clampTextEditState(addSongsSearchEdit_, addSongsSearchQuery_);
                const float addSongsSearchWidth = panel.width - menuPadding * 2.0f;
                const float addSongsSearchScrollOffset = textFieldScrollOffset(
                    addSongsSearchEdit_,
                    addSongsSearchQuery_,
                    addSongsSearchWidth - 20.0f,
                    14.0f);
                addSongsSearchFieldId_ = primitives_.add(Primitive::textField(
                    {
                        .x = panel.x + menuPadding,
                        .y = searchY,
                        .width = addSongsSearchWidth,
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
                        .selectionColor = accent,
                        .selectedTextColor = listBackground,
                        .caretCodepointIndex = addSongsSearchEdit_.caretIndex,
                        .selectionStartCodepointIndex = std::min(addSongsSearchEdit_.caretIndex, addSongsSearchEdit_.selectionAnchor),
                        .selectionEndCodepointIndex = std::max(addSongsSearchEdit_.caretIndex, addSongsSearchEdit_.selectionAnchor),
                        .horizontalScrollOffset = addSongsSearchScrollOffset,
                        .focused = addSongsSearchFocused_,
                        .caretVisible = searchCaretVisible_,
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
    : libraryStore_(LibraryStore::defaultDataDirectory())
    , importService_(libraryStore_)
    , audioPlayer_()
    , playbackQueue_()
    , playbackStats_()
    , mprisService_()
    , window_(1280, 720, "womp")
    , renderer_(window_)
{
    reloadLibrary();
    lastImportDirectory_ = libraryStore_.setting("last_import_directory")
        .transform([](const std::string& value) { return std::filesystem::path{value}; })
        .value_or(homeDirectory());
    numberGroupingSeparator_ = loadNumberGroupingSeparator();
    if (const auto selected = libraryStore_.setting("selected_playlist")) {
        selectedPlaylistId_ = std::strtoll(selected->c_str(), nullptr, 10);
        if (selectedPlaylistId_ != 0
            && std::ranges::find(playlists_, selectedPlaylistId_, &Playlist::id) == playlists_.end()) {
            selectedPlaylistId_ = 0;
        }
    }
    if (const auto volume = libraryStore_.setting("volume")) {
        volume_ = std::clamp(std::strtof(volume->c_str(), nullptr), 0.0f, 1.0f);
    }
    volumeMuted_ = libraryStore_.setting("mute").value_or("0") == "1";
    shuffleEnabled_ = libraryStore_.setting("shuffle").value_or("0") == "1";
    audioPlayer_.setVolume(volume_);
    audioPlayer_.setMuted(volumeMuted_);
    nextStatsFlush_ = std::chrono::steady_clock::now() + std::chrono::seconds{10};
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
    while (window_.pollEvents(eventPollTimeoutMilliseconds(needsDraw), mprisService_.wakeFd())) {
        completePendingAudioScanIfReady();
        completePendingAudioImportIfReady();
        updateSearchCaretBlink();
        handleMprisCommands();
        pollPlayback();

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

    finishPlaylistMetadataEdit(true);
    playbackStats_.finish(false, false, std::chrono::steady_clock::now());
    flushPlaybackStats();
    libraryStore_.setSetting("selected_playlist", std::to_string(selectedPlaylistId_));
    libraryStore_.setSetting("volume", std::to_string(volume_));
    libraryStore_.setSetting("mute", volumeMuted_ ? "1" : "0");
    libraryStore_.setSetting("shuffle", shuffleEnabled_ ? "1" : "0");
    renderer_.waitIdle();
}

PlaylistId App::addPlaylist(std::string name)
{
    const PlaylistId id = libraryStore_.createPlaylist(name);
    const auto maxPosition = std::ranges::max_element(playlists_, {}, &Playlist::position);
    playlists_.push_back({
        .id = id,
        .name = std::move(name),
        .description = "",
        .createdAt = std::chrono::system_clock::now(),
        .position = maxPosition == playlists_.end() ? 0 : maxPosition->position + 1,
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

    if (!libraryStore_.removePlaylist(id)) {
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
        playlistSearchQuery_.clear();
        normalizedPlaylistSearchQuery_.clear();
        filteredPlaylistTrackIndexes_.clear();
        playlistFirstVisibleRow_ = 0;
    }
    std::erase(selectedAddSongsPlaylistIds_, id);

    rebuildScene();
    return true;
}

void App::reloadLibrary()
{
    tracks_.clear();
    trackIndexesById_.clear();
    for (TrackRecord& record : libraryStore_.loadTracks()) {
        trackIndexesById_[record.id] = tracks_.size();
        tracks_.push_back({
            .id = std::move(record.id),
            .path = libraryStore_.absoluteTrackPath(record),
            .title = std::move(record.title),
            .album = std::move(record.album),
            .artists = std::move(record.artists),
            .durationMs = record.durationMs,
            .formatLabel = std::move(record.formatLabel),
            .artworkPath = libraryStore_.absoluteArtworkPath(record),
            .available = record.available,
        });
    }
    playlists_.clear();
    for (PlaylistRecord& record : libraryStore_.loadPlaylists()) {
        Playlist playlist{
            .id = record.id,
            .name = std::move(record.name),
            .description = std::move(record.description),
            .createdAt = std::chrono::system_clock::time_point{std::chrono::milliseconds{record.createdAtMs}},
            .position = record.position,
            .pinned = record.pinned,
            .trackIndexes = std::move(record.trackIds),
        };
        playlist.trackIndexSet.insert(playlist.trackIndexes.begin(), playlist.trackIndexes.end());
        playlists_.push_back(std::move(playlist));
    }
}

std::vector<Primitive> App::renderPrimitives(VulkanRenderer::PrimitiveUpdate update) const
{
    const bool needsText = includesUpdate(update, VulkanRenderer::PrimitiveUpdate::Text);
    const bool needsSvg = includesUpdate(update, VulkanRenderer::PrimitiveUpdate::Svg)
        || includesUpdate(update, VulkanRenderer::PrimitiveUpdate::Image);
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
            .clip = primitive.clip,
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
        } else if (const auto* image = std::get_if<ImagePrimitive>(&primitive.geometry)) {
            snapshot.geometry = ImagePrimitive{
                .x = image->x,
                .y = image->y,
                .width = image->width,
                .height = image->height,
                .source = needsSvg ? image->source : cachedSourceMarker(image->source),
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
                .selectionColor = textField->selectionColor,
                .selectedTextColor = textField->selectedTextColor,
                .caretCodepointIndex = textField->caretCodepointIndex,
                .selectionStartCodepointIndex = textField->selectionStartCodepointIndex,
                .selectionEndCodepointIndex = textField->selectionEndCodepointIndex,
                .horizontalScrollOffset = textField->horizontalScrollOffset,
                .wrapMode = textField->wrapMode,
                .maxVisibleLines = textField->maxVisibleLines,
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
    playlistMetadataField_ = TextFieldTarget::None;
    libraryStore_.setSetting("selected_playlist", std::to_string(id));
    playlistSearchQuery_.clear();
    normalizedPlaylistSearchQuery_.clear();
    filteredPlaylistTrackIndexes_.clear();
    playlistFirstVisibleRow_ = 0;
    clearPlaylistTrackRowHover();
    rebuildScene();
}

void App::createPlaylist()
{
    constexpr std::string_view baseName = "New Playlist";
    std::string name(baseName);
    std::size_t suffix = 2;
    const auto nameExists = [this](std::string_view candidate) {
        return std::ranges::any_of(playlists_, [candidate](const Playlist& playlist) {
            return playlist.name == candidate;
        });
    };
    while (nameExists(name)) {
        name = std::string(baseName) + " " + std::to_string(suffix++);
    }

    selectedPlaylistId_ = libraryStore_.createPlaylist(name);
    const auto maxPosition = std::ranges::max_element(playlists_, {}, &Playlist::position);
    playlists_.push_back({
        .id = selectedPlaylistId_,
        .name = std::move(name),
        .description = "",
        .createdAt = std::chrono::system_clock::now(),
        .position = maxPosition == playlists_.end() ? 0 : maxPosition->position + 1,
    });
    sidebarPlaylistFirstVisibleRow_ = playlists_.size() - 1;
    playlistSearchQuery_.clear();
    normalizedPlaylistSearchQuery_.clear();
    filteredPlaylistTrackIndexes_.clear();
    playlistFirstVisibleRow_ = 0;
    rebuildScene();
}

bool App::setPlaylistPinned(PlaylistId id, bool pinned)
{
    const auto playlist = std::ranges::find(playlists_, id, &Playlist::id);
    if (playlist == playlists_.end() || playlist->pinned == pinned) {
        return false;
    }
    if (!libraryStore_.setPlaylistPinned(id, pinned)) {
        return false;
    }

    playlist->pinned = pinned;
    sortPlaylists();
    sidebarPlaylistFirstVisibleRow_ = 0;
    rebuildScene();
    return true;
}

void App::sortPlaylists()
{
    std::ranges::sort(playlists_, [](const Playlist& left, const Playlist& right) {
        if (left.pinned != right.pinned) {
            return left.pinned > right.pinned;
        }
        if (left.position != right.position) {
            return left.position < right.position;
        }
        return left.id < right.id;
    });
}

void App::setPlaylistSearchQuery(std::string query)
{
    if (playlistSearchQuery_ == query) {
        return;
    }

    playlistSearchQuery_ = std::move(query);
    clampTextEditState(playlistSearchEdit_, playlistSearchQuery_);
    normalizedPlaylistSearchQuery_ = lowercaseAscii(playlistSearchQuery_);
    rebuildPlaylistTrackFilter();
}

void App::rebuildPlaylistTrackFilter()
{
    filteredPlaylistTrackIndexes_.clear();
    playlistFirstVisibleRow_ = 0;
    clearPlaylistTrackRowHover();

    if (normalizedPlaylistSearchQuery_.empty()) {
        return;
    }

    const auto addIfMatching = [this](const TrackId& trackId) {
        const Track* track = findTrack(trackId);
        if (track != nullptr) {
            const std::string searchable = lowercaseAscii(
                track->title + "\n" + track->album + "\n" + joinArtists(track->artists));
            if (searchable.find(normalizedPlaylistSearchQuery_) != std::string::npos) {
                filteredPlaylistTrackIndexes_.push_back(trackId);
            }
        }
    };

    if (selectedPlaylistId_ == 0) {
        for (const Track& track : tracks_) {
            addIfMatching(track.id);
        }
        return;
    }

    const auto playlist = std::ranges::find(playlists_, selectedPlaylistId_, &Playlist::id);
    if (playlist == playlists_.end()) {
        return;
    }
    for (const TrackId& trackId : playlist->trackIndexes) {
        addIfMatching(trackId);
    }
}

const App::Track* App::displayedPlaylistTrack(std::size_t displayedIndex) const
{
    if (!normalizedPlaylistSearchQuery_.empty()) {
        if (displayedIndex >= filteredPlaylistTrackIndexes_.size()) {
            return nullptr;
        }
        return findTrack(filteredPlaylistTrackIndexes_[displayedIndex]);
    }

    if (selectedPlaylistId_ == 0) {
        return displayedIndex < tracks_.size() ? &tracks_[displayedIndex] : nullptr;
    }

    const auto playlist = std::ranges::find(playlists_, selectedPlaylistId_, &Playlist::id);
    if (playlist == playlists_.end() || displayedIndex >= playlist->trackIndexes.size()) {
        return nullptr;
    }

    return findTrack(playlist->trackIndexes[displayedIndex]);
}

std::size_t App::displayedPlaylistTrackCount() const
{
    if (!normalizedPlaylistSearchQuery_.empty()) {
        return filteredPlaylistTrackIndexes_.size();
    }

    if (selectedPlaylistId_ == 0) {
        return tracks_.size();
    }

    const auto playlist = std::ranges::find(playlists_, selectedPlaylistId_, &Playlist::id);
    return playlist == playlists_.end() ? 0 : playlist->trackIndexes.size();
}

std::string App::playlistTrackFileTypeBadge(const Track& track) const
{
    std::string extension = track.formatLabel;
    if (extension.empty()) {
        return "AUDIO";
    }

    std::ranges::transform(extension, extension.begin(), [](unsigned char character) {
        return character >= 'a' && character <= 'z'
            ? static_cast<char>(character - 'a' + 'A')
            : static_cast<char>(character);
    });
    return truncateText(std::move(extension), 6);
}

std::size_t App::playlistTrackMaxFirstVisibleRow() const
{
    if (playlistTrackRows_.empty()) {
        return 0;
    }

    const std::size_t fullyVisibleCapacity = playlistTrackViewportHeight_ >= playlistTrackRowHeight
        ? 1 + static_cast<std::size_t>(
            std::floor((playlistTrackViewportHeight_ - playlistTrackRowHeight) / playlistTrackRowStride))
        : 1;
    const std::size_t trackCount = displayedPlaylistTrackCount();
    return trackCount > fullyVisibleCapacity ? trackCount - fullyVisibleCapacity : 0;
}

void App::setPlaylistFirstVisibleRow(std::size_t firstVisibleRow)
{
    const std::size_t clamped = std::min(firstVisibleRow, playlistTrackMaxFirstVisibleRow());
    if (playlistFirstVisibleRow_ == clamped) {
        return;
    }

    playlistFirstVisibleRow_ = clamped;
    clearPlaylistTrackRowHover();
    refreshPlaylistTrackRows();
}

void App::refreshPlaylistTrackRows()
{
    const Color transparent{0.0f, 0.0f, 0.0f, 0.0f};
    const Color sidebarText = rgb(211, 198, 170);
    const Color iconGrey = rgb(133, 146, 137);
    const Color accent = rgb(167, 192, 128);
    const Color border = rgb(71, 82, 88);
    const Color coverFill = rgb(52, 63, 68);

    const auto setStyleColor = [&](PrimitiveId id, Color fill, Color stroke = Color{0.0f, 0.0f, 0.0f, 0.0f}, float strokeWidth = 0.0f) {
        if (Primitive* primitive = primitives_.find(id)) {
            primitive->style = {.fill = fill, .stroke = stroke, .strokeWidth = strokeWidth};
        }
    };
    const auto setText = [&](PrimitiveId id, std::string text, Color color) {
        if (Primitive* primitive = primitives_.find(id)) {
            if (auto* geometry = std::get_if<TextPrimitive>(&primitive->geometry)) {
                geometry->text = std::move(text);
            }
            primitive->style = {.fill = color, .stroke = color, .strokeWidth = 0.0f};
        }
    };

    for (std::size_t slotIndex = 0; slotIndex < playlistTrackRows_.size(); ++slotIndex) {
        PlaylistTrackRowPrimitives& row = playlistTrackRows_[slotIndex];
        const Track* track = displayedPlaylistTrack(playlistFirstVisibleRow_ + slotIndex);
        row.active = track != nullptr;

        if (!row.active) {
            setStyleColor(row.background, transparent);
            setStyleColor(row.coverTile, transparent);
            setStyleColor(row.coverIcon, transparent);
            setText(row.title, "", transparent);
            setStyleColor(row.fileTypeBadgeBackground, transparent);
            setText(row.fileTypeBadge, "", transparent);
            setText(row.album, "", transparent);
            setText(row.artist, "", transparent);
            setText(row.duration, "", transparent);
            setText(row.hoveredDuration, "", transparent);
        } else {
            Primitive* titlePrimitive = primitives_.find(row.title);
            Primitive* badgePrimitive = primitives_.find(row.fileTypeBadge);
            const auto* titleGeometry = titlePrimitive == nullptr ? nullptr : std::get_if<TextPrimitive>(&titlePrimitive->geometry);
            auto* badgeGeometry = badgePrimitive == nullptr ? nullptr : std::get_if<TextPrimitive>(&badgePrimitive->geometry);
            const auto* albumGeometry = [&]() -> const TextPrimitive* {
                const Primitive* primitive = primitives_.find(row.album);
                return primitive == nullptr ? nullptr : std::get_if<TextPrimitive>(&primitive->geometry);
            }();
            const auto* artistGeometry = [&]() -> const TextPrimitive* {
                const Primitive* primitive = primitives_.find(row.artist);
                return primitive == nullptr ? nullptr : std::get_if<TextPrimitive>(&primitive->geometry);
            }();
            const float titleRight = artistGeometry == nullptr ? 0.0f : artistGeometry->x - 20.0f;
            const std::string badge = playlistTrackFileTypeBadge(*track);
            const float badgeTextWidth = static_cast<float>(badge.size()) * playlistTrackFileTypeBadgeCharacterWidth;
            const float badgeWidth = badgeTextWidth
                + playlistTrackFileTypeBadgeHorizontalPadding * 2.0f;
            const float titleWidth = titleGeometry == nullptr
                ? 0.0f
                : std::max(0.0f, titleRight - titleGeometry->x - badgeWidth - playlistTrackFileTypeBadgeGap);
            const float albumWidth = titleGeometry == nullptr || albumGeometry == nullptr
                ? 0.0f
                : std::max(0.0f, titleRight - albumGeometry->x);
            const float artistRight = [&]() {
                const Primitive* primitive = primitives_.find(row.hoveredDuration);
                const auto* geometry = primitive == nullptr ? nullptr : std::get_if<TextPrimitive>(&primitive->geometry);
                return geometry == nullptr ? 0.0f : geometry->x - 18.0f;
            }();
            const float artistWidth = artistGeometry == nullptr ? 0.0f : std::max(0.0f, artistRight - artistGeometry->x);
            const std::string title = fitTextToWidth(
                track->title,
                titleWidth,
                playlistTrackTitleCharacterWidth);
            const bool badgeVisible = titleWidth > 0.0f;

            const bool current = playbackQueue_.current() && *playbackQueue_.current() == track->id;
            setStyleColor(row.background, current ? rgb(63, 74, 69, 0.62f) : transparent);
            setStyleColor(row.coverTile, coverFill, border, 1.0f);
            if (Primitive* cover = primitives_.find(row.coverIcon)) {
                const Primitive* tile = primitives_.find(row.coverTile);
                const auto* tileGeometry = tile == nullptr ? nullptr : std::get_if<RoundedRectPrimitive>(&tile->geometry);
                if (!track->artworkPath.empty() && tileGeometry != nullptr) {
                    cover->geometry = ImagePrimitive{
                        .x = tileGeometry->x,
                        .y = tileGeometry->y,
                        .width = tileGeometry->width,
                        .height = tileGeometry->height,
                        .source = track->artworkPath.string(),
                    };
                    cover->style = {.fill = {1.0f, 1.0f, 1.0f, 1.0f}};
                } else {
                    cover->geometry = ImagePrimitive{};
                    cover->style = {.fill = transparent};
                }
            }
            setText(row.title, title, sidebarText);
            setStyleColor(
                row.fileTypeBadgeBackground,
                badgeVisible ? rgb(71, 82, 88, 0.45f) : transparent,
                badgeVisible ? border : transparent,
                badgeVisible ? 1.0f : 0.0f);
            setText(row.fileTypeBadge, badgeVisible ? badge : std::string{}, accent);
            setText(row.album, fitTextToWidth(track->album, albumWidth, 7.0f), iconGrey);
            setText(row.artist, fitTextToWidth(joinArtists(track->artists), artistWidth, 7.4f), iconGrey);
            setText(row.duration, formatDurationMs(track->durationMs), sidebarText);
            setText(row.hoveredDuration, formatDurationMs(track->durationMs), transparent);
            if (titleGeometry != nullptr && badgeGeometry != nullptr) {
                const float titleVisualWidth = renderer_.measureTextVisualWidth(
                    title,
                    titleGeometry->fontFamilies,
                    titleGeometry->fontSize);
                const float badgeX = std::min(
                    titleRight - badgeWidth,
                    titleGeometry->x + titleVisualWidth + playlistTrackFileTypeBadgeGap);
                badgeGeometry->x = badgeX + (badgeWidth - badgeTextWidth) * 0.5f;
                if (Primitive* primitive = primitives_.find(row.fileTypeBadgeBackground)) {
                    if (auto* background = std::get_if<RoundedRectPrimitive>(&primitive->geometry)) {
                        background->x = badgeX;
                        background->width = badgeVisible ? badgeWidth : 0.0f;
                    }
                }
            }
        }

        for (const PrimitiveId buttonId : {row.playButton, row.ellipsisButton}) {
            if (Primitive* primitive = primitives_.find(buttonId)) {
                if (auto* button = std::get_if<ButtonPrimitive>(&primitive->geometry)) {
                    button->enabled = false;
                    button->hovered = false;
                    button->pressed = false;
                    button->iconColor = transparent;
                    if (buttonId == row.playButton && track != nullptr) {
                        const TrackId id = track->id;
                        const bool current = playbackQueue_.current() && *playbackQueue_.current() == id;
                        button->iconSvg = current && playing_ ? pauseIconSvg() : playIconSvg();
                        button->onClick = [this, id] { toggleTrackPlayback(id); };
                    }
                }
                primitive->style.fill = transparent;
                primitive->style.stroke = transparent;
            }
        }
    }

    const std::size_t trackCount = displayedPlaylistTrackCount();
    const float totalTrackHeight = trackCount == 0
        ? 0.0f
        : static_cast<float>(trackCount) * playlistTrackRowStride - playlistTrackRowGap;
    const bool scrollbarVisible = playlistTrackMaxFirstVisibleRow() > 0
        && playlistScrollbarTrackHeight_ > 0.0f;
    playlistScrollbarThumbHeight_ = scrollbarVisible
        ? std::min(
            playlistScrollbarTrackHeight_,
            std::max(28.0f, playlistScrollbarTrackHeight_ * playlistTrackViewportHeight_ / totalTrackHeight))
        : 0.0f;
    const float thumbTravel = std::max(0.0f, playlistScrollbarTrackHeight_ - playlistScrollbarThumbHeight_);
    const std::size_t maxFirstVisibleRow = playlistTrackMaxFirstVisibleRow();
    const float scrollRatio = maxFirstVisibleRow == 0
        ? 0.0f
        : static_cast<float>(playlistFirstVisibleRow_) / static_cast<float>(maxFirstVisibleRow);
    playlistScrollbarThumbY_ = playlistScrollbarTrackY_ + thumbTravel * scrollRatio;
    if (Primitive* primitive = primitives_.find(playlistScrollbarTrackId_)) {
        primitive->style.fill = scrollbarVisible ? rgb(71, 82, 88, 0.55f) : transparent;
    }
    if (Primitive* primitive = primitives_.find(playlistScrollbarThumbId_)) {
        if (auto* thumb = std::get_if<RoundedRectPrimitive>(&primitive->geometry)) {
            thumb->y = playlistScrollbarThumbY_;
            thumb->height = playlistScrollbarThumbHeight_;
        }
        primitive->style.fill = scrollbarVisible ? iconGrey : transparent;
    }

    refreshPrimitives(VulkanRenderer::PrimitiveUpdate::Full);
}

void App::refreshPlaylistTrackRowHover(std::size_t hoveredSlot)
{
    const Color transparent{0.0f, 0.0f, 0.0f, 0.0f};
    const Color sidebarText = rgb(211, 198, 170);
    const Color rowHoverFill = rgb(52, 63, 68, 0.72f);
    const Color buttonIcon = rgb(211, 198, 170);
    hoveredPlaylistTrackSlot_ = hoveredSlot;

    for (std::size_t slotIndex = 0; slotIndex < playlistTrackRows_.size(); ++slotIndex) {
        PlaylistTrackRowPrimitives& row = playlistTrackRows_[slotIndex];
        const bool hovered = row.active && slotIndex == hoveredSlot;
        const Track* track = displayedPlaylistTrack(playlistFirstVisibleRow_ + slotIndex);
        const bool current = track != nullptr && playbackQueue_.current() && *playbackQueue_.current() == track->id;
        if (Primitive* primitive = primitives_.find(row.background)) {
            primitive->style.fill = hovered ? rowHoverFill : (current ? rgb(63, 74, 69, 0.62f) : transparent);
        }
        if (Primitive* primitive = primitives_.find(row.duration)) {
            primitive->style.fill = hovered ? transparent : sidebarText;
            primitive->style.stroke = primitive->style.fill;
        }
        if (Primitive* primitive = primitives_.find(row.hoveredDuration)) {
            primitive->style.fill = hovered ? sidebarText : transparent;
            primitive->style.stroke = primitive->style.fill;
        }
        for (const PrimitiveId buttonId : {row.playButton, row.ellipsisButton}) {
            if (Primitive* primitive = primitives_.find(buttonId)) {
                if (auto* button = std::get_if<ButtonPrimitive>(&primitive->geometry)) {
                    const bool enabled = hovered
                        && (buttonId != row.playButton || (track != nullptr && track->available));
                    button->enabled = enabled;
                    button->iconColor = enabled ? buttonIcon : transparent;
                    if (buttonId == row.playButton && track != nullptr) {
                        button->iconSvg = current && playing_ ? pauseIconSvg() : playIconSvg();
                    }
                    if (!hovered) {
                        button->hovered = false;
                        button->pressed = false;
                    }
                }
            }
        }
    }
    refreshPrimitives(VulkanRenderer::PrimitiveUpdate::DrawOnly);
}

void App::clearPlaylistTrackRowHover()
{
    if (hoveredPlaylistTrackSlot_ != static_cast<std::size_t>(-1)) {
        refreshPlaylistTrackRowHover(static_cast<std::size_t>(-1));
    }
}

bool App::playlistTrackViewportContains(float x, float y) const
{
    return x >= playlistTrackViewportX_ && x <= playlistTrackViewportX_ + playlistTrackViewportWidth_
        && y >= playlistTrackViewportY_ && y <= playlistTrackViewportY_ + playlistTrackViewportHeight_;
}

std::size_t App::sidebarPlaylistMaxFirstVisibleRow() const
{
    const float contentHeight = playlists_.empty()
        ? 0.0f
        : static_cast<float>(playlists_.size()) * sidebarPlaylistRowStride
            - sidebarPlaylistButtonGap
            + sidebarPlaylistEndPadding;
    const float maxScrollOffset = std::max(0.0f, contentHeight - sidebarPlaylistViewportHeight_);
    return static_cast<std::size_t>(std::ceil(maxScrollOffset / sidebarPlaylistRowStride));
}

void App::setSidebarPlaylistFirstVisibleRow(std::size_t firstVisibleRow)
{
    const std::size_t clamped = std::min(firstVisibleRow, sidebarPlaylistMaxFirstVisibleRow());
    if (sidebarPlaylistFirstVisibleRow_ == clamped) {
        return;
    }

    sidebarPlaylistFirstVisibleRow_ = clamped;
    rebuildScene();
}

bool App::sidebarPlaylistViewportContains(float x, float y) const
{
    return x >= sidebarPlaylistViewportX_
        && x <= sidebarPlaylistViewportX_ + sidebarPlaylistViewportWidth_
        && y >= sidebarPlaylistViewportY_
        && y <= sidebarPlaylistViewportY_ + sidebarPlaylistViewportHeight_;
}

bool App::addTrackToPlaylist(PlaylistId playlistId, const TrackId& trackId)
{
    if (playlistId == 0 || !trackIndexesById_.contains(trackId)) {
        return false;
    }

    const auto playlist = std::ranges::find(playlists_, playlistId, &Playlist::id);
    if (playlist == playlists_.end() || !playlist->trackIndexSet.insert(trackId).second) {
        return false;
    }

    if (!libraryStore_.addTrackToPlaylist(playlistId, trackId)) {
        playlist->trackIndexSet.erase(trackId);
        return false;
    }
    playlist->trackIndexes.push_back(trackId);
    return true;
}

const App::Track* App::findTrack(const TrackId& id) const
{
    const auto found = trackIndexesById_.find(id);
    return found == trackIndexesById_.end() || found->second >= tracks_.size()
        ? nullptr
        : &tracks_[found->second];
}

App::Track* App::findTrack(const TrackId& id)
{
    return const_cast<Track*>(std::as_const(*this).findTrack(id));
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
    draggingSearchSelection_ = TextFieldTarget::None;
    searchCaretVisible_ = true;
    nextSearchCaretBlink_ = std::chrono::steady_clock::now() + searchCaretBlinkInterval;
    draggingPendingSongsScrollbar_ = false;
    pendingSongsScrollbarDragOffsetY_ = 0.0f;
    pendingAddSongsScrollOffset_ = 0.0f;
    if (clearPendingSongs) {
        resetPendingSongs();
        selectedAddSongsPlaylistIds_.clear();
        addSongsSearchQuery_.clear();
        normalizedAddSongsSearchQuery_.clear();
        addSongsSearchEdit_ = {};
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
    clampTextEditState(addSongsSearchEdit_, addSongsSearchQuery_);
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

void App::resetSearchCaretBlink()
{
    searchCaretVisible_ = true;
    nextSearchCaretBlink_ = std::chrono::steady_clock::now() + searchCaretBlinkInterval;

    for (const PrimitiveId searchFieldId : {
             addSongsSearchFieldId_,
             playlistSearchFieldId_,
             playlistTitleFieldId_,
             playlistDescriptionFieldId_}) {
        if (Primitive* primitive = primitives_.find(searchFieldId)) {
            if (auto* textField = std::get_if<TextFieldPrimitive>(&primitive->geometry); textField != nullptr && !textField->caretVisible) {
                textField->caretVisible = true;
                refreshPrimitives();
            }
        }
    }
}

void App::updateSearchCaretBlink()
{
    PrimitiveId activeSearchFieldId = 0;
    if (addSongsMenuOpen_ && addSongsSearchFocused_ && !addSongsAudioScanActive_) {
        activeSearchFieldId = addSongsSearchFieldId_;
    } else if (!anyModalOpen() && playlistSearchFocused_) {
        activeSearchFieldId = playlistSearchFieldId_;
    } else if (!anyModalOpen() && playlistMetadataField_ == TextFieldTarget::PlaylistTitle) {
        activeSearchFieldId = playlistTitleFieldId_;
    } else if (!anyModalOpen() && playlistMetadataField_ == TextFieldTarget::PlaylistDescription) {
        activeSearchFieldId = playlistDescriptionFieldId_;
    }
    if (activeSearchFieldId == 0) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now < nextSearchCaretBlink_) {
        return;
    }

    searchCaretVisible_ = !searchCaretVisible_;
    nextSearchCaretBlink_ = now + searchCaretBlinkInterval;
    if (Primitive* primitive = primitives_.find(activeSearchFieldId)) {
        if (auto* textField = std::get_if<TextFieldPrimitive>(&primitive->geometry)) {
            textField->caretVisible = searchCaretVisible_;
            refreshPrimitives();
        }
    }
}

std::int32_t App::eventPollTimeoutMilliseconds(bool needsDraw) const
{
    if (addSongsAudioScanActive_ || audioImportActive_) {
        return 16;
    }
    if (needsDraw) {
        return 0;
    }
    if (playing_) {
        return 100;
    }
    const bool searchFieldFocused = (addSongsMenuOpen_ && addSongsSearchFocused_)
        || (!anyModalOpen() && (playlistSearchFocused_ || playlistMetadataField_ != TextFieldTarget::None));
    if (!searchFieldFocused) {
        return -1;
    }

    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        nextSearchCaretBlink_ - std::chrono::steady_clock::now());
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
    createPlaylistMenuOpen_ = false;
    addSongsMenuOpen_ = !addSongsMenuOpen_;
    if (!addSongsMenuOpen_) {
        resetAddSongsMenuState(true);
    } else {
        playlistSearchFocused_ = false;
        resetAddSongsMenuState(false);
    }
    rebuildScene();
}

void App::toggleCreatePlaylistMenu()
{
    createPlaylistMenuOpen_ = !createPlaylistMenuOpen_;
    if (createPlaylistMenuOpen_) {
        addSongsMenuOpen_ = false;
        resetAddSongsMenuState(true);
        playlistSearchFocused_ = false;
        draggingSearchSelection_ = TextFieldTarget::None;
    }
    rebuildScene();
}

void App::closeCreatePlaylistMenu()
{
    if (!createPlaylistMenuOpen_) {
        return;
    }

    createPlaylistMenuOpen_ = false;
    rebuildScene();
}

void App::beginImportPlaylistFiles()
{
    if (audioImportActive_) {
        return;
    }
    std::vector<std::filesystem::path> paths = runPlaylistImportDialog(lastImportDirectory_);
    if (paths.empty()) {
        return;
    }

    updateLastImportDirectory(paths);
    importPlaylistFiles(paths);
}

void App::importPlaylistFiles(const std::vector<std::filesystem::path>& paths)
{
    if (paths.empty() || audioImportActive_) {
        return;
    }

    std::vector<std::pair<std::filesystem::path, PlaylistId>> playlists;
    playlists.reserve(paths.size());
    for (const std::filesystem::path& path : paths) {
        const PlaylistId playlistId = libraryStore_.createPlaylist(displayNameForPath(path));
        playlists.emplace_back(path, playlistId);
        selectedPlaylistId_ = playlistId;
    }

    audioImportActive_ = true;
    playlistImportActive_ = true;
    createPlaylistMenuOpen_ = false;
    lastImportResultText_ = "Importing " + std::to_string(paths.size()) + " playlist"
        + (paths.size() == 1 ? "..." : "s...");
    pendingAudioImport_ = std::async(
        std::launch::async,
        [this, playlists = std::move(playlists)]() {
            ImportResult combined;
            for (const auto& [path, playlistId] : playlists) {
                const ImportResult result = importService_.importM3u(path, playlistId);
                combined.imported += result.imported;
                combined.duplicates += result.duplicates;
                combined.unsupported += result.unsupported;
                combined.failed += result.failed;
            }
            return combined;
        });
    rebuildScene();
}

void App::exportPlaylist(PlaylistId id)
{
    const auto playlist = std::ranges::find(playlists_, id, &Playlist::id);
    if (playlist == playlists_.end()) {
        return;
    }

    const std::filesystem::path path = runPlaylistExportDialog(lastImportDirectory_, playlist->name);
    if (path.empty()) {
        return;
    }

    if (!libraryStore_.exportPlaylistM3u(id, path)) {
        const std::string command = "zenity --error --title='Export failed' --text="
            + shellQuote("Could not export the playlist to " + path.string());
        std::system(command.c_str());
        return;
    }

    lastImportDirectory_ = path.parent_path();
    libraryStore_.setSetting("last_import_directory", lastImportDirectory_.string());
}

void App::beginPlaylistMetadataEdit(TextFieldTarget target)
{
    if (target != TextFieldTarget::PlaylistTitle && target != TextFieldTarget::PlaylistDescription) {
        return;
    }
    if (playlistMetadataField_ == target) {
        return;
    }
    if (playlistMetadataField_ != TextFieldTarget::None) {
        finishPlaylistMetadataEdit(true);
    }

    const auto playlist = std::ranges::find(playlists_, selectedPlaylistId_, &Playlist::id);
    if (playlist == playlists_.end()) {
        return;
    }

    playlistMetadataField_ = target;
    playlistTitleDraft_ = playlist->name;
    playlistDescriptionDraft_ = playlist->description;
    playlistTitleEdit_ = {
        .caretIndex = playlistTitleDraft_.size(),
        .selectionAnchor = playlistTitleDraft_.size(),
    };
    playlistDescriptionEdit_ = {
        .caretIndex = playlistDescriptionDraft_.size(),
        .selectionAnchor = playlistDescriptionDraft_.size(),
    };
    addSongsSearchFocused_ = false;
    playlistSearchFocused_ = false;
    resetSearchCaretBlink();
    rebuildScene();
}

void App::finishPlaylistMetadataEdit(bool save)
{
    if (playlistMetadataField_ == TextFieldTarget::None) {
        return;
    }

    const auto playlist = std::ranges::find(playlists_, selectedPlaylistId_, &Playlist::id);
    if (save && playlist != playlists_.end()) {
        const bool hasVisibleTitle = std::ranges::any_of(playlistTitleDraft_, [](unsigned char character) {
            return std::isspace(character) == 0;
        });
        if (!hasVisibleTitle) {
            playlistTitleDraft_ = playlist->name;
        }
        if (libraryStore_.updatePlaylistMetadata(
                playlist->id,
                playlistTitleDraft_,
                playlistDescriptionDraft_)) {
            playlist->name = playlistTitleDraft_;
            playlist->description = playlistDescriptionDraft_;
        }
    }

    playlistMetadataField_ = TextFieldTarget::None;
    playlistTitleDraft_.clear();
    playlistDescriptionDraft_.clear();
    playlistTitleEdit_ = {};
    playlistDescriptionEdit_ = {};
    if (sceneReady_) {
        rebuildScene();
    }
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
    if (audioImportActive_) {
        return;
    }
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

void App::completePendingAudioImportIfReady()
{
    if (!audioImportActive_ || !pendingAudioImport_.valid()
        || pendingAudioImport_.wait_for(std::chrono::seconds{0}) != std::future_status::ready) {
        return;
    }

    const bool importedPlaylists = playlistImportActive_;
    audioImportActive_ = false;
    playlistImportActive_ = false;
    if (!importedPlaylists) {
        addSongsMenuOpen_ = true;
        resetAddSongsMenuState(true);
    }
    try {
        const ImportResult result = pendingAudioImport_.get();
        lastImportResultText_ = importResultText(result);
        reloadLibrary();
        if (!normalizedPlaylistSearchQuery_.empty()) {
            rebuildPlaylistTrackFilter();
        }
    } catch (const std::exception& error) {
        lastImportResultText_ = "Import failed: " + std::string(error.what());
    } catch (...) {
        lastImportResultText_ = "Import failed";
    }
    rebuildScene();
}

void App::addPendingSongs()
{
    if (pendingAddSongs_.empty() || audioImportActive_) {
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

    std::vector<std::filesystem::path> paths;
    paths.reserve(songs.size());
    for (PendingAudioFile& song : songs) {
        paths.push_back(std::move(song.path));
    }
    audioImportActive_ = true;
    addSongsMenuOpen_ = true;
    lastImportResultText_ = "Importing " + std::to_string(paths.size()) + " songs...";
    pendingAudioImport_ = std::async(
        std::launch::async,
        [this, paths = std::move(paths), playlistIds = std::move(playlistIds)]() mutable {
            return importService_.importFiles(std::move(paths), playlistIds);
        });
    rebuildScene();
}

void App::importFiles(const std::vector<std::filesystem::path>& paths)
{
    if (paths.empty()) {
        return;
    }

    ImportResult combined;
    const auto mergeResult = [&combined](const ImportResult& result) {
        combined.imported += result.imported;
        combined.duplicates += result.duplicates;
        combined.unsupported += result.unsupported;
        combined.failed += result.failed;
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
                mergeResult(importService_.importFiles(audioFiles, {playlistId}));
            } else {
                mergeResult(importService_.importFiles(audioFiles, selectedPlaylistId_ == 0 ? std::vector<PlaylistId>{} : std::vector<PlaylistId>{selectedPlaylistId_}));
            }
            continue;
        }
        mergeResult(importService_.importFiles({normalizedPath}, selectedPlaylistId_ == 0 ? std::vector<PlaylistId>{} : std::vector<PlaylistId>{selectedPlaylistId_}));
    }
    lastImportResultText_ = importResultText(combined);
    reloadLibrary();
    addSongsMenuOpen_ = false;
    resetAddSongsMenuState(true);
    if (!normalizedPlaylistSearchQuery_.empty()) {
        rebuildPlaylistTrackFilter();
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
    playlistSearchFocused_ = false;
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
            libraryStore_.setSetting("last_import_directory", lastImportDirectory_.string());
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

bool App::anyModalOpen() const
{
    return createPlaylistMenuOpen_ || addSongsMenuOpen_;
}

bool App::activeModalContains(float x, float y) const
{
    if (createPlaylistMenuOpen_) {
        return createPlaylistMenuContains(x, y);
    }
    return addSongsMenuOpen_ && addSongsMenuContains(x, y);
}

bool App::createPlaylistMenuContains(float x, float y) const
{
    return contains(
        createPlaylistMenuRect(
            static_cast<float>(window_.width()),
            static_cast<float>(window_.height())),
        x,
        y);
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

bool App::playlistSearchFieldContains(float x, float y) const
{
    if (anyModalOpen()) {
        return false;
    }

    const Primitive* primitive = primitives_.find(playlistSearchFieldId_);
    const auto* searchField = primitive == nullptr
        ? nullptr
        : std::get_if<TextFieldPrimitive>(&primitive->geometry);
    return searchField != nullptr && contains(*searchField, x, y);
}

bool App::playlistTitleFieldContains(float x, float y) const
{
    return playlistMetadataFieldContains(playlistTitleFieldId_, x, y);
}

bool App::playlistDescriptionFieldContains(float x, float y) const
{
    return playlistMetadataFieldContains(playlistDescriptionFieldId_, x, y);
}

bool App::playlistMetadataFieldContains(PrimitiveId id, float x, float y) const
{
    if (anyModalOpen()) {
        return false;
    }
    const Primitive* primitive = primitives_.find(id);
    const auto* field = primitive == nullptr
        ? nullptr
        : std::get_if<TextFieldPrimitive>(&primitive->geometry);
    if (field == nullptr) {
        return false;
    }

    return contains(*field, x, y);
}

void App::togglePlayback()
{
    if (!hasCurrentSong_) {
        playSelectedSource();
        return;
    }

    if (playing_) {
        audioPlayer_.pause();
        playbackStats_.pause(std::chrono::steady_clock::now());
        flushPlaybackStats();
        playing_ = false;
    } else {
        audioPlayer_.resume();
        playing_ = true;
    }
    updateMpris();
    rebuildScene();
}

void App::toggleTrackPlayback(const TrackId& trackId)
{
    if (playbackQueue_.current() && *playbackQueue_.current() == trackId) {
        togglePlayback();
        return;
    }
    playTrack(trackId);
}

void App::toggleShuffle()
{
    shuffleEnabled_ = !shuffleEnabled_;
    playbackQueue_.setShuffle(shuffleEnabled_);
    libraryStore_.setSetting("shuffle", shuffleEnabled_ ? "1" : "0");
    updateMpris();
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

    audioPlayer_.setVolume(volume_);
    audioPlayer_.setMuted(volumeMuted_);
    libraryStore_.setSetting("mute", volumeMuted_ ? "1" : "0");
    updateMpris();
    refreshVolumeControl();
}

void App::setVolumeFromPointer(float x)
{
    volume_ = std::clamp((x - volumeSliderX_) / volumeSliderWidth_, 0.0f, 1.0f);
    if (volume_ > 0.0f) {
        volumeBeforeMute_ = volume_;
    }
    volumeMuted_ = false;
    audioPlayer_.setVolume(volume_);
    audioPlayer_.setMuted(false);
    libraryStore_.setSetting("volume", std::to_string(volume_));
    libraryStore_.setSetting("mute", "0");
    updateMpris();
    refreshVolumeControl();
}

void App::setMediaProgressFromPointer(float x)
{
    if (!canSeekMediaProgress()) {
        return;
    }

    const float progress = std::clamp((x - mediaProgressSliderX_) / mediaProgressSliderWidth_, 0.0f, 1.0f);
    currentSongElapsedSeconds_ = currentSongDurationSeconds_ * progress;
    audioPlayer_.seek(static_cast<std::int64_t>(currentSongElapsedSeconds_ * 1000.0f));
    playbackStats_.seek(std::chrono::steady_clock::now());
    mprisService_.emitSeeked(static_cast<std::int64_t>(currentSongElapsedSeconds_ * 1'000'000.0f));
    updateMpris();
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
    return hasCurrentSong_ && currentSongDurationSeconds_ > 0.0f;
}

std::vector<TrackId> App::selectedSourceTrackIds() const
{
    if (selectedPlaylistId_ == 0) {
        std::vector<TrackId> ids;
        ids.reserve(tracks_.size());
        for (const Track& track : tracks_) {
            ids.push_back(track.id);
        }
        return ids;
    }
    const auto playlist = std::ranges::find(playlists_, selectedPlaylistId_, &Playlist::id);
    return playlist == playlists_.end() ? std::vector<TrackId>{} : playlist->trackIndexes;
}

bool App::selectedSourceIsCurrent() const
{
    const std::optional<PlaylistId> selectedSourceId = selectedPlaylistId_ == 0
        ? std::nullopt
        : std::optional<PlaylistId>{selectedPlaylistId_};
    return playbackQueue_.current() && playbackQueue_.sourcePlaylistId() == selectedSourceId;
}

void App::playTrack(const TrackId& trackId)
{
    std::vector<TrackId> source = selectedSourceTrackIds();
    if (std::ranges::find(source, trackId) == source.end()) {
        return;
    }
    if (playbackStats_.active()) {
        playbackStats_.finish(false, playbackQueue_.current() != trackId, std::chrono::steady_clock::now());
        flushPlaybackStats();
    }
    playbackQueue_.start(
        std::move(source),
        selectedPlaylistId_ == 0 ? std::nullopt : std::optional<PlaylistId>{selectedPlaylistId_},
        trackId,
        shuffleEnabled_);
    startCurrentTrack();
}

void App::playSelectedSource()
{
    const std::vector<TrackId> source = selectedSourceTrackIds();
    if (source.empty()) {
        return;
    }
    if (selectedSourceIsCurrent()) {
        audioPlayer_.resume();
        playing_ = true;
        updateMpris();
        rebuildScene();
        return;
    }
    if (playbackStats_.active()) {
        playbackStats_.finish(false, true, std::chrono::steady_clock::now());
        flushPlaybackStats();
    }
    playbackQueue_.start(
        source,
        selectedPlaylistId_ == 0 ? std::nullopt : std::optional<PlaylistId>{selectedPlaylistId_},
        shuffleEnabled_ ? TrackId{} : source.front(),
        shuffleEnabled_);
    startCurrentTrack();
}

void App::startCurrentTrack()
{
    while (playbackQueue_.current()) {
        const Track* track = findTrack(*playbackQueue_.current());
        if (track != nullptr && track->available && audioPlayer_.play(track->path)) {
            hasCurrentSong_ = true;
            playing_ = true;
            currentSongElapsedSeconds_ = 0.0f;
            currentSongDurationSeconds_ = static_cast<float>(track->durationMs) / 1000.0f;
            playbackStats_.start(
                track->id,
                playbackQueue_.sourcePlaylistId(),
                track->durationMs,
                currentTimeMs(),
                std::chrono::steady_clock::now());
            updateMpris();
            rebuildScene();
            return;
        }
        playbackQueue_.next();
    }
    hasCurrentSong_ = false;
    playing_ = false;
    currentSongElapsedSeconds_ = 0.0f;
    currentSongDurationSeconds_ = 0.0f;
    updateMpris();
    rebuildScene();
}

void App::playNext(bool userInitiated)
{
    if (!playbackQueue_.current()) {
        return;
    }
    playbackStats_.finish(false, userInitiated, std::chrono::steady_clock::now());
    flushPlaybackStats();
    playbackQueue_.next();
    startCurrentTrack();
}

void App::playPrevious()
{
    if (!playbackQueue_.current()) {
        return;
    }
    if (audioPlayer_.positionMs() > 3000) {
        audioPlayer_.seek(0);
        playbackStats_.seek(std::chrono::steady_clock::now());
        return;
    }
    if (playbackQueue_.history().empty()) {
        audioPlayer_.seek(0);
        playbackStats_.seek(std::chrono::steady_clock::now());
        return;
    }
    playbackStats_.finish(false, true, std::chrono::steady_clock::now());
    flushPlaybackStats();
    if (playbackQueue_.previous()) {
        startCurrentTrack();
    }
}

void App::flushPlaybackStats()
{
    const std::vector<StatisticDelta> deltas = playbackStats_.takeDeltas(std::chrono::steady_clock::now());
    libraryStore_.applyStatisticDeltas(deltas);
    nextStatsFlush_ = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    if (!deltas.empty() && sceneReady_) {
        rebuildScene();
    }
}

void App::pollPlayback()
{
    const auto now = std::chrono::steady_clock::now();
    playbackStats_.tick(audioPlayer_.actuallyPlaying(), now);
    if (now >= nextStatsFlush_) {
        flushPlaybackStats();
    }
    refreshListenedTime();
    for (const AudioPlayer::Event& event : audioPlayer_.pollEvents()) {
        if (event.type == AudioPlayer::EventType::Eos) {
            playbackStats_.finish(true, false, now);
            flushPlaybackStats();
            playbackQueue_.next();
            startCurrentTrack();
            return;
        }
        if (event.type == AudioPlayer::EventType::Error) {
            playbackStats_.finish(false, false, now);
            flushPlaybackStats();
            playbackQueue_.next();
            startCurrentTrack();
            return;
        }
    }
    if (hasCurrentSong_) {
        const float position = static_cast<float>(audioPlayer_.positionMs()) / 1000.0f;
        const float duration = static_cast<float>(audioPlayer_.durationMs()) / 1000.0f;
        if (!draggingMediaProgressSlider_) {
            currentSongElapsedSeconds_ = position;
            if (duration > 0.0f) currentSongDurationSeconds_ = duration;
            refreshMediaProgressControl();
        }
        mprisService_.updatePosition(audioPlayer_.positionMs() * 1000);
    }
}

void App::handleMprisCommands()
{
    for (const MprisService::Command& command : mprisService_.takeCommands()) {
        switch (command.type) {
        case MprisService::CommandType::Play: if (!playing_) togglePlayback(); break;
        case MprisService::CommandType::Pause: if (playing_) togglePlayback(); break;
        case MprisService::CommandType::PlayPause: togglePlayback(); break;
        case MprisService::CommandType::Next: playNext(true); break;
        case MprisService::CommandType::Previous: playPrevious(); break;
        case MprisService::CommandType::Seek:
            audioPlayer_.seek(audioPlayer_.positionMs() + command.positionUs / 1000);
            playbackStats_.seek(std::chrono::steady_clock::now());
            mprisService_.emitSeeked(audioPlayer_.positionMs() * 1000);
            break;
        case MprisService::CommandType::SetPosition:
            audioPlayer_.seek(command.positionUs / 1000);
            playbackStats_.seek(std::chrono::steady_clock::now());
            mprisService_.emitSeeked(command.positionUs);
            break;
        case MprisService::CommandType::SetShuffle:
            if (shuffleEnabled_ != command.boolean) toggleShuffle();
            break;
        case MprisService::CommandType::SetVolume:
            volume_ = std::clamp(static_cast<float>(command.number), 0.0f, 1.0f);
            volumeMuted_ = false;
            audioPlayer_.setVolume(volume_);
            audioPlayer_.setMuted(false);
            libraryStore_.setSetting("volume", std::to_string(volume_));
            libraryStore_.setSetting("mute", "0");
            updateMpris();
            refreshVolumeControl();
            break;
        }
    }
}

void App::updateMpris()
{
    std::optional<MprisService::Metadata> metadata;
    if (playbackQueue_.current()) {
        if (const Track* track = findTrack(*playbackQueue_.current())) {
            metadata = MprisService::Metadata{
                .trackId = track->id,
                .title = track->title,
                .album = track->album,
                .artists = track->artists,
                .durationUs = track->durationMs * 1000,
                .artworkPath = track->artworkPath,
            };
        }
    }
    mprisService_.update(
        hasCurrentSong_ ? (playing_ ? "Playing" : "Paused") : "Stopped",
        audioPlayer_.positionMs() * 1000,
        volumeMuted_ ? 0.0 : volume_,
        shuffleEnabled_,
        std::move(metadata));
}

bool App::confirmPlaylistDeletion(PlaylistId id) const
{
    const auto playlist = std::ranges::find(playlists_, id, &Playlist::id);
    if (playlist == playlists_.end()) {
        return false;
    }
    const std::string command = "zenity --question --title='Delete Playlist' --text="
        + shellQuote("Delete playlist \"" + playlist->name + "\"? Songs will remain in your library.");
    return std::system(command.c_str()) == 0;
}

void App::clampTextEditState(TextEditState& state, const std::string& text) const
{
    state.caretIndex = std::min(state.caretIndex, text.size());
    state.selectionAnchor = std::min(state.selectionAnchor, text.size());
}

float App::textFieldScrollOffset(
    TextEditState& state,
    const std::string& text,
    float contentWidth,
    float fontSize) const
{
    if (contentWidth <= 0.0f || text.empty()) {
        state.horizontalScrollOffset = 0.0f;
        return 0.0f;
    }

    const std::size_t caretIndex = std::min(state.caretIndex, text.size());
    const float caretX = renderer_.measureTextVisualWidth(text.substr(0, caretIndex), {}, fontSize);
    const float textWidth = renderer_.measureTextVisualWidth(text, {}, fontSize);
    const float visibleWidth = std::max(0.0f, contentWidth - 1.0f);
    const float maxScrollOffset = std::max(0.0f, textWidth - visibleWidth);
    state.horizontalScrollOffset = std::clamp(state.horizontalScrollOffset, 0.0f, maxScrollOffset);
    if (caretX < state.horizontalScrollOffset) {
        state.horizontalScrollOffset = caretX;
    } else if (caretX > state.horizontalScrollOffset + visibleWidth) {
        state.horizontalScrollOffset = caretX - visibleWidth;
    }
    return state.horizontalScrollOffset;
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
    bool drawChanged = false;

    if (Primitive* primitive = primitives_.find(mediaProgressSliderFillId_)) {
        if (auto* fill = std::get_if<RoundedRectPrimitive>(&primitive->geometry)) {
            const float width = mediaProgressSliderWidth_ * progress;
            if (std::abs(fill->width - width) >= 0.5f) {
                fill->width = width;
                drawChanged = true;
            }
        }
    }

    if (Primitive* primitive = primitives_.find(mediaProgressSliderKnobId_)) {
        if (auto* knob = std::get_if<CirclePrimitive>(&primitive->geometry)) {
            const float centerX = mediaProgressSliderX_ + mediaProgressSliderWidth_ * progress;
            const Color fill = canSeekMediaProgress() ? rgb(211, 198, 170) : rgb(133, 146, 137);
            if (std::abs(knob->centerX - centerX) >= 0.5f) {
                knob->centerX = centerX;
                drawChanged = true;
            }
            if (primitive->style.fill.r != fill.r
                || primitive->style.fill.g != fill.g
                || primitive->style.fill.b != fill.b
                || primitive->style.fill.a != fill.a) {
                primitive->style.fill = fill;
                drawChanged = true;
            }
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

    if (textChanged || drawChanged) {
        refreshPrimitives(textChanged ? VulkanRenderer::PrimitiveUpdate::Text : VulkanRenderer::PrimitiveUpdate::DrawOnly);
    }
}

void App::refreshListenedTime()
{
    Primitive* primitive = primitives_.find(listenedTimeTextId_);
    auto* text = primitive == nullptr ? nullptr : std::get_if<TextPrimitive>(&primitive->geometry);
    if (text == nullptr) {
        return;
    }

    const std::optional<PlaylistId> selectedPlaylistScope = selectedPlaylistId_ == 0
        ? std::nullopt
        : std::optional<PlaylistId>{selectedPlaylistId_};
    const std::string listenedTime = truncateText(
        formatSummaryDuration(
            selectedPersistedListenedMs_ + playbackStats_.pendingListenedMsFor(selectedPlaylistScope)),
        listenedTimeMaxCharacters_);
    if (text->text != listenedTime) {
        text->text = listenedTime;
        refreshPrimitives(VulkanRenderer::PrimitiveUpdate::Text);
    }
}

void App::rebuildScene()
{
    primitives_.clear();
    firstModalPrimitiveId_ = 0;
    playlistTrackRows_.clear();
    draggingPlaylistScrollbar_ = false;
    playlistScrollbarTrackId_ = 0;
    playlistScrollbarThumbId_ = 0;
    hoveredPlaylistTrackSlot_ = static_cast<std::size_t>(-1);
    audioScanStatusTextId_ = 0;
    audioScanPathTextId_ = 0;
    audioScanProgressFillId_ = 0;
    addSongsSearchFieldId_ = 0;
    playlistSearchFieldId_ = 0;
    playlistTitleFieldId_ = 0;
    playlistDescriptionFieldId_ = 0;
    listenedTimeTextId_ = 0;
    listenedTimeMaxCharacters_ = 0;
    buildInitialScene(static_cast<float>(window_.width()), static_cast<float>(window_.height()));
    pressedButton_ = 0;
    sceneReady_ = true;
    refreshPrimitives(VulkanRenderer::PrimitiveUpdate::Full);
}

void App::handlePointerEvent(const WaylandWindow::PointerEvent& event)
{
    if (event.type == WaylandWindow::PointerEventType::ButtonPress) {
        draggingMediaProgressSlider_ = false;
        draggingVolumeSlider_ = false;
    }

    if (addSongsAudioScanActive_) {
        window_.setCursor(WaylandWindow::CursorShape::Default);
        return;
    }

    const bool hoveringAddSongsSearchField = event.type != WaylandWindow::PointerEventType::Leave
        && addSongsSearchFieldContains(event.x, event.y);
    const bool hoveringPlaylistSearchField = event.type != WaylandWindow::PointerEventType::Leave
        && playlistSearchFieldContains(event.x, event.y);
    const bool hoveringPlaylistTitleField = event.type != WaylandWindow::PointerEventType::Leave
        && playlistTitleFieldContains(event.x, event.y);
    const bool hoveringPlaylistDescriptionField = event.type != WaylandWindow::PointerEventType::Leave
        && playlistDescriptionFieldContains(event.x, event.y);
    const bool hoveringTextField = hoveringAddSongsSearchField
        || hoveringPlaylistSearchField
        || hoveringPlaylistTitleField
        || hoveringPlaylistDescriptionField;

    const auto updateDraggedSearchSelection = [&](TextFieldTarget field) {
        PrimitiveId primitiveId = 0;
        const std::string* query = nullptr;
        TextEditState* edit = nullptr;
        switch (field) {
        case TextFieldTarget::AddSongsSearch:
            primitiveId = addSongsSearchFieldId_;
            query = &addSongsSearchQuery_;
            edit = &addSongsSearchEdit_;
            break;
        case TextFieldTarget::PlaylistSearch:
            primitiveId = playlistSearchFieldId_;
            query = &playlistSearchQuery_;
            edit = &playlistSearchEdit_;
            break;
        case TextFieldTarget::PlaylistTitle:
            primitiveId = playlistTitleFieldId_;
            query = &playlistTitleDraft_;
            edit = &playlistTitleEdit_;
            break;
        case TextFieldTarget::PlaylistDescription:
            primitiveId = playlistDescriptionFieldId_;
            query = &playlistDescriptionDraft_;
            edit = &playlistDescriptionEdit_;
            break;
        case TextFieldTarget::None:
            return;
        }
        const Primitive* primitive = primitives_.find(primitiveId);
        const auto* textField = primitive == nullptr
            ? nullptr
            : std::get_if<TextFieldPrimitive>(&primitive->geometry);
        if (textField != nullptr && query != nullptr && edit != nullptr) {
            edit->caretIndex = renderer_.textFieldCaretIndexAtPoint(*textField, event.x, event.y);
        }
    };

    if (draggingSearchSelection_ != TextFieldTarget::None) {
        if (event.type == WaylandWindow::PointerEventType::Move
            || event.type == WaylandWindow::PointerEventType::ButtonRelease) {
            updateDraggedSearchSelection(draggingSearchSelection_);
            resetSearchCaretBlink();
            if (event.type == WaylandWindow::PointerEventType::ButtonRelease) {
                draggingSearchSelection_ = TextFieldTarget::None;
            }
            rebuildScene();
            window_.setCursor(hoveringTextField ? WaylandWindow::CursorShape::Text : WaylandWindow::CursorShape::Default);
            return;
        }
        if (event.type == WaylandWindow::PointerEventType::Leave) {
            draggingSearchSelection_ = TextFieldTarget::None;
        }
    }

    if (event.type == WaylandWindow::PointerEventType::ButtonPress && hoveringTextField) {
        const TextFieldTarget field = hoveringAddSongsSearchField
            ? TextFieldTarget::AddSongsSearch
            : (hoveringPlaylistSearchField
                    ? TextFieldTarget::PlaylistSearch
                    : (hoveringPlaylistTitleField
                            ? TextFieldTarget::PlaylistTitle
                            : TextFieldTarget::PlaylistDescription));
        if (field == TextFieldTarget::PlaylistTitle || field == TextFieldTarget::PlaylistDescription) {
            beginPlaylistMetadataEdit(field);
        } else {
            finishPlaylistMetadataEdit(true);
            addSongsSearchFocused_ = field == TextFieldTarget::AddSongsSearch;
            playlistSearchFocused_ = field == TextFieldTarget::PlaylistSearch;
        }
        updateDraggedSearchSelection(field);
        TextEditState& edit = field == TextFieldTarget::AddSongsSearch
            ? addSongsSearchEdit_
            : (field == TextFieldTarget::PlaylistSearch
                    ? playlistSearchEdit_
                    : (field == TextFieldTarget::PlaylistTitle
                            ? playlistTitleEdit_
                            : playlistDescriptionEdit_));
        edit.selectionAnchor = edit.caretIndex;
        draggingSearchSelection_ = field;
        resetSearchCaretBlink();
        rebuildScene();
        window_.setCursor(WaylandWindow::CursorShape::Text);
        return;
    }

    if (event.type == WaylandWindow::PointerEventType::ButtonPress
        && playlistMetadataField_ != TextFieldTarget::None) {
        finishPlaylistMetadataEdit(true);
    }

    if (anyModalOpen()
        && event.type == WaylandWindow::PointerEventType::ButtonPress
        && !activeModalContains(event.x, event.y)) {
        if (createPlaylistMenuOpen_) {
            closeCreatePlaylistMenu();
        } else {
            closeAddSongsMenu();
        }
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

    if (addSongsMenuOpen_ && event.type == WaylandWindow::PointerEventType::ButtonPress) {
        const bool searchFocused = addSongsSearchFieldContains(event.x, event.y);
        if (addSongsSearchFocused_ != searchFocused) {
            addSongsSearchFocused_ = searchFocused;
            resetSearchCaretBlink();
            rebuildScene();
        }
    }

    const float sidebarPlaylistScrollbarHitX = sidebarPlaylistScrollbarTrackX_
        - (sidebarPlaylistScrollbarHitWidth - sidebarPlaylistScrollbarTrackWidth_) * 0.5f;
    const Rect sidebarPlaylistScrollbarHitTarget{
        .x = sidebarPlaylistScrollbarHitX,
        .y = sidebarPlaylistScrollbarTrackY_,
        .width = sidebarPlaylistScrollbarHitWidth,
        .height = sidebarPlaylistScrollbarTrackHeight_,
    };
    const Rect sidebarPlaylistScrollbarThumb{
        .x = sidebarPlaylistScrollbarHitX,
        .y = sidebarPlaylistScrollbarThumbY_,
        .width = sidebarPlaylistScrollbarHitWidth,
        .height = sidebarPlaylistScrollbarThumbHeight_,
    };
    const bool sidebarPlaylistScrollbarVisible = sidebarPlaylistScrollbarThumbHeight_ > 0.0f;
    const auto setSidebarPlaylistScrollFromPointer = [&](float pointerY) {
        const float thumbTravel = std::max(
            0.0f,
            sidebarPlaylistScrollbarTrackHeight_ - sidebarPlaylistScrollbarThumbHeight_);
        const std::size_t maxFirstVisibleRow = sidebarPlaylistMaxFirstVisibleRow();
        if (!sidebarPlaylistScrollbarVisible || thumbTravel <= 0.0f || maxFirstVisibleRow == 0) {
            return;
        }

        const float thumbY = std::clamp(
            pointerY - sidebarPlaylistScrollbarDragOffsetY_,
            sidebarPlaylistScrollbarTrackY_,
            sidebarPlaylistScrollbarTrackY_ + thumbTravel);
        const float scrollRatio = (thumbY - sidebarPlaylistScrollbarTrackY_) / thumbTravel;
        setSidebarPlaylistFirstVisibleRow(static_cast<std::size_t>(std::round(
            scrollRatio * static_cast<float>(maxFirstVisibleRow))));
    };

    if (draggingSidebarPlaylistScrollbar_) {
        if (event.type == WaylandWindow::PointerEventType::Move) {
            setSidebarPlaylistScrollFromPointer(event.y);
            window_.setCursor(WaylandWindow::CursorShape::Pointer);
            return;
        }
        if (event.type == WaylandWindow::PointerEventType::ButtonRelease) {
            setSidebarPlaylistScrollFromPointer(event.y);
            draggingSidebarPlaylistScrollbar_ = false;
            window_.setCursor(WaylandWindow::CursorShape::Pointer);
            return;
        }
        if (event.type == WaylandWindow::PointerEventType::Leave) {
            draggingSidebarPlaylistScrollbar_ = false;
        }
    }

    const bool hoveringSidebarPlaylistScrollbar = !anyModalOpen()
        && sidebarPlaylistScrollbarVisible
        && event.type != WaylandWindow::PointerEventType::Leave
        && contains(sidebarPlaylistScrollbarHitTarget, event.x, event.y);
    if (event.type == WaylandWindow::PointerEventType::ButtonPress && hoveringSidebarPlaylistScrollbar) {
        draggingSidebarPlaylistScrollbar_ = true;
        sidebarPlaylistScrollbarDragOffsetY_ = contains(sidebarPlaylistScrollbarThumb, event.x, event.y)
            ? event.y - sidebarPlaylistScrollbarThumbY_
            : sidebarPlaylistScrollbarThumbHeight_ * 0.5f;
        setSidebarPlaylistScrollFromPointer(event.y);
        window_.setCursor(WaylandWindow::CursorShape::Pointer);
        return;
    }

    if (!anyModalOpen()
        && event.type == WaylandWindow::PointerEventType::Scroll
        && event.scrollY != 0.0f
        && sidebarPlaylistScrollbarVisible
        && sidebarPlaylistViewportContains(event.x, event.y)) {
        if (event.scrollY > 0.0f) {
            setSidebarPlaylistFirstVisibleRow(sidebarPlaylistFirstVisibleRow_ + 1);
        } else if (sidebarPlaylistFirstVisibleRow_ > 0) {
            setSidebarPlaylistFirstVisibleRow(sidebarPlaylistFirstVisibleRow_ - 1);
        }
        return;
    }

    if (!anyModalOpen() && event.type == WaylandWindow::PointerEventType::ButtonPress) {
        const bool searchFocused = playlistSearchFieldContains(event.x, event.y);
        if (playlistSearchFocused_ != searchFocused) {
            playlistSearchFocused_ = searchFocused;
            resetSearchCaretBlink();
            if (Primitive* primitive = primitives_.find(playlistSearchFieldId_)) {
                if (auto* textField = std::get_if<TextFieldPrimitive>(&primitive->geometry)) {
                    textField->focused = playlistSearchFocused_;
                    textField->caretVisible = searchCaretVisible_;
                }
                primitive->style.stroke = playlistSearchFocused_ ? rgb(167, 192, 128) : rgb(71, 82, 88);
            }
            refreshPrimitives();
        }
    }

    const float playlistScrollbarHitX = playlistScrollbarTrackX_
        - (playlistTrackScrollbarHitWidth - playlistScrollbarTrackWidth_) * 0.5f;
    const Rect playlistScrollbarHitTarget{
        .x = playlistScrollbarHitX,
        .y = playlistScrollbarTrackY_,
        .width = playlistTrackScrollbarHitWidth,
        .height = playlistScrollbarTrackHeight_,
    };
    const Rect playlistScrollbarThumb{
        .x = playlistScrollbarHitX,
        .y = playlistScrollbarThumbY_,
        .width = playlistTrackScrollbarHitWidth,
        .height = playlistScrollbarThumbHeight_,
    };
    const bool playlistScrollbarVisible = playlistScrollbarThumbHeight_ > 0.0f;
    const auto setPlaylistScrollFromPointer = [&](float pointerY) {
        const float thumbTravel = std::max(0.0f, playlistScrollbarTrackHeight_ - playlistScrollbarThumbHeight_);
        const std::size_t maxFirstVisibleRow = playlistTrackMaxFirstVisibleRow();
        if (!playlistScrollbarVisible || thumbTravel <= 0.0f || maxFirstVisibleRow == 0) {
            return;
        }

        const float thumbY = std::clamp(
            pointerY - playlistScrollbarDragOffsetY_,
            playlistScrollbarTrackY_,
            playlistScrollbarTrackY_ + thumbTravel);
        const float scrollRatio = (thumbY - playlistScrollbarTrackY_) / thumbTravel;
        setPlaylistFirstVisibleRow(static_cast<std::size_t>(std::round(
            scrollRatio * static_cast<float>(maxFirstVisibleRow))));
    };

    if (draggingPlaylistScrollbar_) {
        if (event.type == WaylandWindow::PointerEventType::Move) {
            setPlaylistScrollFromPointer(event.y);
            window_.setCursor(WaylandWindow::CursorShape::Pointer);
            return;
        }
        if (event.type == WaylandWindow::PointerEventType::ButtonRelease) {
            setPlaylistScrollFromPointer(event.y);
            draggingPlaylistScrollbar_ = false;
            window_.setCursor(WaylandWindow::CursorShape::Pointer);
            return;
        }
        if (event.type == WaylandWindow::PointerEventType::Leave) {
            draggingPlaylistScrollbar_ = false;
        }
    }

    const bool hoveringPlaylistScrollbar = !anyModalOpen()
        && playlistScrollbarVisible
        && event.type != WaylandWindow::PointerEventType::Leave
        && contains(playlistScrollbarHitTarget, event.x, event.y);
    if (event.type == WaylandWindow::PointerEventType::ButtonPress && hoveringPlaylistScrollbar) {
        draggingPlaylistScrollbar_ = true;
        playlistScrollbarDragOffsetY_ = contains(playlistScrollbarThumb, event.x, event.y)
            ? event.y - playlistScrollbarThumbY_
            : playlistScrollbarThumbHeight_ * 0.5f;
        setPlaylistScrollFromPointer(event.y);
        window_.setCursor(WaylandWindow::CursorShape::Pointer);
        return;
    }

    if (!anyModalOpen()
        && event.type == WaylandWindow::PointerEventType::Scroll
        && event.scrollY != 0.0f
        && playlistTrackViewportContains(event.x, event.y)) {
        if (event.scrollY > 0.0f) {
            setPlaylistFirstVisibleRow(playlistFirstVisibleRow_ + 1);
        } else if (playlistFirstVisibleRow_ > 0) {
            setPlaylistFirstVisibleRow(playlistFirstVisibleRow_ - 1);
        }
        return;
    }

    std::size_t hoveredPlaylistSlot = static_cast<std::size_t>(-1);
    if (!anyModalOpen()
        && event.type != WaylandWindow::PointerEventType::Leave
        && playlistTrackViewportContains(event.x, event.y)) {
        const float relativeY = event.y - playlistTrackViewportY_;
        const std::size_t slot = static_cast<std::size_t>(relativeY / playlistTrackRowStride);
        const float yWithinStride = relativeY - static_cast<float>(slot) * playlistTrackRowStride;
        if (slot < playlistTrackRows_.size() && yWithinStride <= playlistTrackRowHeight && playlistTrackRows_[slot].active) {
            hoveredPlaylistSlot = slot;
        }
    }
    if (hoveredPlaylistTrackSlot_ != hoveredPlaylistSlot) {
        refreshPlaylistTrackRowHover(hoveredPlaylistSlot);
    }

    const bool hoveringMediaProgressSlider = !anyModalOpen()
        && event.type != WaylandWindow::PointerEventType::Leave
        && canSeekMediaProgress()
        && mediaProgressSliderContains(event.x, event.y);
    const bool hoveringVolumeSlider = !anyModalOpen()
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
    const auto pointerInsidePrimitiveClip = [&event](const Primitive& primitive) {
        if (!primitive.clip) {
            return true;
        }
        return event.x >= primitive.clip->x
            && event.x <= primitive.clip->x + primitive.clip->width
            && event.y >= primitive.clip->y
            && event.y <= primitive.clip->y + primitive.clip->height;
    };

    for (Primitive& primitive : primitives_.all()) {
        auto* button = std::get_if<ButtonPrimitive>(&primitive.geometry);
        if (button == nullptr) {
            continue;
        }

        const bool isModalButton = firstModalPrimitiveId_ != 0
            && primitive.id >= firstModalPrimitiveId_;
        const bool canInteract = primitive.visible
            && button->enabled
            && (!anyModalOpen() || isModalButton)
            && pointerInsidePrimitiveClip(primitive);
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

    window_.setCursor(
        hoveringTextField
            ? WaylandWindow::CursorShape::Text
            : (hasHoveredButton || hoveringPendingSongsScrollbar || hoveringSidebarPlaylistScrollbar || hoveringPlaylistScrollbar || hoveringMediaProgressSlider || hoveringVolumeSlider
                    ? WaylandWindow::CursorShape::Pointer
                    : WaylandWindow::CursorShape::Default));

    if (changed) {
        refreshPrimitives();
    }

    if (event.type == WaylandWindow::PointerEventType::ButtonRelease
        && releasedButton == 0
        && hoveredPlaylistSlot != static_cast<std::size_t>(-1)) {
        const Track* track = displayedPlaylistTrack(playlistFirstVisibleRow_ + hoveredPlaylistSlot);
        if (track != nullptr) {
            constexpr std::uint32_t doubleClickIntervalMs = 400;
            const bool doubleClick = track->id == lastClickedPlaylistTrackId_
                && event.timeMs - lastPlaylistTrackClickTimeMs_ <= doubleClickIntervalMs;
            lastClickedPlaylistTrackId_ = track->id;
            lastPlaylistTrackClickTimeMs_ = event.timeMs;
            if (doubleClick) {
                lastClickedPlaylistTrackId_.clear();
                toggleTrackPlayback(track->id);
                return;
            }
        }
    }

    if (releasedButton != 0) {
        if (Primitive* primitive = primitives_.find(releasedButton)) {
            if (auto* button = std::get_if<ButtonPrimitive>(&primitive->geometry); button != nullptr && button->onClick) {
                // Callbacks may rebuild the scene and destroy the button that owns them.
                const std::function<void()> onClick = button->onClick;
                onClick();

                bool hasPostClickHoveredButton = false;
                for (Primitive& postClickPrimitive : primitives_.all()) {
                    auto* postClickButton = std::get_if<ButtonPrimitive>(&postClickPrimitive.geometry);
                    if (postClickButton == nullptr) {
                        continue;
                    }

                    const bool isModalButton = firstModalPrimitiveId_ != 0
                        && postClickPrimitive.id >= firstModalPrimitiveId_;
                    const bool canInteract = postClickPrimitive.visible
                        && postClickButton->enabled
                        && (!anyModalOpen() || isModalButton)
                        && pointerInsidePrimitiveClip(postClickPrimitive);
                    postClickButton->hovered = canInteract && contains(*postClickButton, event.x, event.y);
                    hasPostClickHoveredButton = hasPostClickHoveredButton || postClickButton->hovered;
                }

                window_.setCursor(
                    hoveringTextField
                        ? WaylandWindow::CursorShape::Text
                        : (hasPostClickHoveredButton || hoveringPendingSongsScrollbar || hoveringSidebarPlaylistScrollbar || hoveringPlaylistScrollbar || hoveringMediaProgressSlider || hoveringVolumeSlider
                                ? WaylandWindow::CursorShape::Pointer
                                : WaylandWindow::CursorShape::Default));
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

    if (event.key == KEY_PLAYPAUSE) {
        togglePlayback();
        return;
    }

    if (addSongsAudioScanActive_) {
        return;
    }

    if (event.key == KEY_ESC) {
        draggingSearchSelection_ = TextFieldTarget::None;
        if (createPlaylistMenuOpen_) {
            closeCreatePlaylistMenu();
        } else if (addSongsMenuOpen_) {
            closeAddSongsMenu();
        } else if (playlistMetadataField_ != TextFieldTarget::None) {
            finishPlaylistMetadataEdit(false);
        } else if (playlistSearchFocused_) {
            playlistSearchFocused_ = false;
            rebuildScene();
        }
        return;
    }

    TextFieldTarget target = TextFieldTarget::None;
    if (addSongsSearchFocused_) {
        target = TextFieldTarget::AddSongsSearch;
    } else if (playlistSearchFocused_) {
        target = TextFieldTarget::PlaylistSearch;
    } else if (playlistMetadataField_ != TextFieldTarget::None) {
        target = playlistMetadataField_;
    }
    if (target == TextFieldTarget::None) {
        return;
    }

    resetSearchCaretBlink();

    TextEditState* edit = target == TextFieldTarget::AddSongsSearch
        ? &addSongsSearchEdit_
        : (target == TextFieldTarget::PlaylistSearch
                ? &playlistSearchEdit_
                : (target == TextFieldTarget::PlaylistTitle
                        ? &playlistTitleEdit_
                        : &playlistDescriptionEdit_));
    std::string query = target == TextFieldTarget::AddSongsSearch
        ? addSongsSearchQuery_
        : (target == TextFieldTarget::PlaylistSearch
                ? playlistSearchQuery_
                : (target == TextFieldTarget::PlaylistTitle
                        ? playlistTitleDraft_
                        : playlistDescriptionDraft_));
    clampTextEditState(*edit, query);

    const auto commit = [&](bool textChanged) {
        if (textChanged) {
            if (target == TextFieldTarget::AddSongsSearch) {
                setAddSongsSearchQuery(std::move(query));
            } else if (target == TextFieldTarget::PlaylistSearch) {
                setPlaylistSearchQuery(std::move(query));
            } else if (target == TextFieldTarget::PlaylistTitle) {
                playlistTitleDraft_ = std::move(query);
            } else {
                playlistDescriptionDraft_ = std::move(query);
            }
        }
        rebuildScene();
    };

    const auto eraseSelection = [&]() {
        const std::size_t start = std::min(edit->caretIndex, edit->selectionAnchor);
        const std::size_t end = std::max(edit->caretIndex, edit->selectionAnchor);
        if (start == end) {
            return false;
        }
        query.erase(start, end - start);
        edit->caretIndex = start;
        edit->selectionAnchor = start;
        return true;
    };

    const auto descriptionFits = [&](const std::string& candidate) {
        const Primitive* primitive = primitives_.find(playlistDescriptionFieldId_);
        const auto* field = primitive == nullptr
            ? nullptr
            : std::get_if<TextFieldPrimitive>(&primitive->geometry);
        return field == nullptr || renderer_.textFieldTextFits(*field, candidate);
    };

    if (event.control && event.key == KEY_A) {
        edit->selectionAnchor = 0;
        edit->caretIndex = query.size();
        commit(false);
        return;
    }

    if (event.key == KEY_ENTER || event.key == KEY_KPENTER) {
        if (target == TextFieldTarget::PlaylistTitle
            || (target == TextFieldTarget::PlaylistDescription && event.control)) {
            finishPlaylistMetadataEdit(true);
            return;
        }
        if (target == TextFieldTarget::PlaylistDescription) {
            const TextEditState originalEdit = *edit;
            eraseSelection();
            query.insert(edit->caretIndex, 1, '\n');
            ++edit->caretIndex;
            edit->selectionAnchor = edit->caretIndex;
            if (descriptionFits(query)) {
                commit(true);
            } else {
                *edit = originalEdit;
            }
        }
        return;
    }

    const bool verticalDescriptionNavigation = target == TextFieldTarget::PlaylistDescription
        && (event.key == KEY_UP || event.key == KEY_DOWN);
    if (event.key == KEY_LEFT
        || event.key == KEY_RIGHT
        || event.key == KEY_HOME
        || event.key == KEY_END
        || verticalDescriptionNavigation) {
        const bool hasSelection = edit->caretIndex != edit->selectionAnchor;
        if (!event.shift && hasSelection && (event.key == KEY_LEFT || event.key == KEY_RIGHT)) {
            edit->caretIndex = event.key == KEY_LEFT
                ? std::min(edit->caretIndex, edit->selectionAnchor)
                : std::max(edit->caretIndex, edit->selectionAnchor);
        } else {
            switch (event.key) {
            case KEY_LEFT:
                edit->caretIndex = event.control
                    ? previousWordBoundary(query, edit->caretIndex)
                    : (edit->caretIndex > 0 ? edit->caretIndex - 1 : 0);
                break;
            case KEY_RIGHT:
                edit->caretIndex = event.control
                    ? nextWordBoundary(query, edit->caretIndex)
                    : std::min(query.size(), edit->caretIndex + 1);
                break;
            case KEY_UP:
            case KEY_DOWN: {
                const Primitive* primitive = primitives_.find(playlistDescriptionFieldId_);
                const auto* field = primitive == nullptr
                    ? nullptr
                    : std::get_if<TextFieldPrimitive>(&primitive->geometry);
                if (field != nullptr) {
                    edit->caretIndex = renderer_.textFieldCaretIndexOnAdjacentLine(
                        *field,
                        edit->caretIndex,
                        event.key == KEY_UP ? -1 : 1);
                }
                break;
            }
            case KEY_HOME:
                if (target == TextFieldTarget::PlaylistDescription && !event.control) {
                    const Primitive* primitive = primitives_.find(playlistDescriptionFieldId_);
                    const auto* field = primitive == nullptr
                        ? nullptr
                        : std::get_if<TextFieldPrimitive>(&primitive->geometry);
                    edit->caretIndex = field == nullptr
                        ? 0
                        : renderer_.textFieldVisualLineStart(*field, edit->caretIndex);
                } else {
                    edit->caretIndex = 0;
                }
                break;
            case KEY_END:
                if (target == TextFieldTarget::PlaylistDescription && !event.control) {
                    const Primitive* primitive = primitives_.find(playlistDescriptionFieldId_);
                    const auto* field = primitive == nullptr
                        ? nullptr
                        : std::get_if<TextFieldPrimitive>(&primitive->geometry);
                    edit->caretIndex = field == nullptr
                        ? query.size()
                        : renderer_.textFieldVisualLineEnd(*field, edit->caretIndex);
                } else {
                    edit->caretIndex = query.size();
                }
                break;
            default:
                break;
            }
        }
        if (!event.shift) {
            edit->selectionAnchor = edit->caretIndex;
        }
        commit(false);
        return;
    }

    if (event.key == KEY_BACKSPACE || event.key == KEY_DELETE) {
        if (eraseSelection()) {
            commit(true);
            return;
        }

        if (event.key == KEY_BACKSPACE && edit->caretIndex > 0) {
            const std::size_t start = event.control
                ? previousWordBoundary(query, edit->caretIndex)
                : edit->caretIndex - 1;
            query.erase(start, edit->caretIndex - start);
            edit->caretIndex = start;
            edit->selectionAnchor = start;
            commit(true);
        } else if (event.key == KEY_DELETE && edit->caretIndex < query.size()) {
            const std::size_t end = event.control
                ? nextWordBoundary(query, edit->caretIndex)
                : edit->caretIndex + 1;
            query.erase(edit->caretIndex, end - edit->caretIndex);
            edit->selectionAnchor = edit->caretIndex;
            commit(true);
        }
        return;
    }

    char character = (!event.control && !event.alt)
        ? characterForKey(event.key, event.shift)
        : '\0';
    if (character != '\0') {
        const TextEditState originalEdit = *edit;
        eraseSelection();
        if (target == TextFieldTarget::PlaylistTitle
            && query.size() >= playlistTitleCharacterLimit) {
            *edit = originalEdit;
            return;
        }
        query.insert(edit->caretIndex, 1, character);
        ++edit->caretIndex;
        edit->selectionAnchor = edit->caretIndex;
        if (target == TextFieldTarget::PlaylistDescription) {
            const std::size_t characterCount = static_cast<std::size_t>(std::ranges::count_if(query, [](char value) {
                return value != '\n';
            }));
            if (characterCount > playlistDescriptionCharacterLimit || !descriptionFits(query)) {
                *edit = originalEdit;
                return;
            }
        }
        commit(true);
    }
}

void App::refreshPrimitives(VulkanRenderer::PrimitiveUpdate update)
{
    pendingPrimitiveUpdate_ = combineUpdates(pendingPrimitiveUpdate_, update);
    primitivesDirty_ = true;
}

} // namespace womp
