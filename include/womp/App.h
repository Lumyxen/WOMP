#pragma once

#include "womp/platform/WaylandWindow.h"
#include "womp/renderer/VulkanRenderer.h"
#include "womp/scene/Primitive.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace womp {

struct AudioScanProgress;

struct PendingAudioFile {
    std::filesystem::path path;
    std::string displayName;
    std::string normalizedDisplayName;
};

class App {
public:
    using PlaylistId = std::uint64_t;

    App();

    void run();
    PlaylistId addPlaylist(std::string name);
    bool removePlaylist(PlaylistId id);

private:
    struct Playlist {
        PlaylistId id = 0;
        std::string name;
        std::vector<std::size_t> trackIndexes;
        std::unordered_set<std::size_t> trackIndexSet;
    };

    struct Track {
        std::filesystem::path path;
        std::string title;
    };

    enum class DirectoryImportMode {
        Playlist,
        Files,
    };

    void selectPlaylist(PlaylistId id);
    bool addTrackToPlaylist(PlaylistId playlistId, std::size_t trackIndex);
    void toggleAddSongsMenu();
    void closeAddSongsMenu();
    void beginImportFiles();
    void completePendingAudioScanIfReady();
    void addPendingSongs();
    void importPendingSongs(std::vector<PendingAudioFile> songs, std::vector<PlaylistId> playlistIds);
    void importFiles(const std::vector<std::filesystem::path>& paths);
    void startPendingAudioScan(std::vector<std::filesystem::path> paths);
    void addPendingAudioFiles(std::vector<PendingAudioFile> songs);
    void updateLastImportDirectory(const std::vector<std::filesystem::path>& paths);
    void setDirectoryImportMode(DirectoryImportMode mode);
    void toggleAddSongsPlaylistDropdown();
    void toggleAddSongsPlaylistSelection(PlaylistId id);
    void resetPendingSongs();
    void resetAddSongsMenuState(bool clearPendingSongs);
    void setAddSongsSearchQuery(std::string query);
    void rebuildPendingSongFilter();
    std::size_t pendingSongMatchCount() const;
    void refreshAudioScanProgress();
    void resetAddSongsCaretBlink();
    void updateAddSongsCaretBlink();
    std::int32_t eventPollTimeoutMilliseconds(bool needsDraw) const;
    void togglePlayback();
    void toggleShuffle();
    void toggleVolumeMute();
    void setVolumeFromPointer(float x);
    void setMediaProgressFromPointer(float x);
    void rebuildScene();
    void buildInitialScene(float windowWidth, float windowHeight);
    void handlePointerEvent(const WaylandWindow::PointerEvent& event);
    void handleKeyEvent(const WaylandWindow::KeyEvent& event);
    void refreshPrimitives(VulkanRenderer::PrimitiveUpdate update = VulkanRenderer::PrimitiveUpdate::DrawOnly);
    std::vector<Primitive> renderPrimitives(VulkanRenderer::PrimitiveUpdate update) const;
    bool addSongsMenuContains(float x, float y) const;
    bool addSongsSearchFieldContains(float x, float y) const;
    bool mediaProgressSliderContains(float x, float y) const;
    bool volumeSliderContains(float x, float y) const;
    bool canSeekMediaProgress() const;
    void refreshVolumeControl();
    void refreshMediaProgressControl();

    WaylandWindow window_;
    VulkanRenderer renderer_;
    PrimitiveStore primitives_;
    std::vector<Playlist> playlists_;
    std::vector<Track> tracks_;
    std::unordered_map<std::filesystem::path, std::size_t> trackIndexesByPath_;
    PlaylistId nextPlaylistId_ = 1;
    PlaylistId selectedPlaylistId_ = 0;
    PrimitiveId pressedButton_ = 0;
    bool addSongsMenuOpen_ = false;
    bool addSongsSearchFocused_ = false;
    bool addSongsDirectoryOptionsVisible_ = false;
    bool addSongsPlaylistDropdownOpen_ = false;
    bool addSongsCaretVisible_ = true;
    bool addSongsAudioScanActive_ = false;
    std::chrono::steady_clock::time_point nextAddSongsCaretBlink_ = std::chrono::steady_clock::now();
    std::future<std::vector<PendingAudioFile>> pendingAudioScan_;
    std::shared_ptr<AudioScanProgress> pendingAudioScanProgress_;
    std::uint64_t renderedAudioScanProgressRevision_ = 0;
    float pendingAddSongsScrollOffset_ = 0.0f;
    std::vector<PendingAudioFile> pendingAddSongs_;
    std::vector<std::size_t> filteredPendingSongIndexes_;
    std::vector<PlaylistId> selectedAddSongsPlaylistIds_;
    std::string addSongsSearchQuery_;
    std::string normalizedAddSongsSearchQuery_;
    std::filesystem::path lastImportDirectory_;
    char numberGroupingSeparator_ = ',';
    DirectoryImportMode directoryImportMode_ = DirectoryImportMode::Playlist;
    bool playing_ = false;
    bool hasCurrentSong_ = false;
    float currentSongElapsedSeconds_ = 0.0f;
    float currentSongDurationSeconds_ = 0.0f;
    bool shuffleEnabled_ = true;
    float volume_ = 1.0f;
    float volumeBeforeMute_ = 1.0f;
    bool volumeMuted_ = false;
    bool draggingPendingSongsScrollbar_ = false;
    float pendingSongsScrollbarDragOffsetY_ = 0.0f;
    bool draggingMediaProgressSlider_ = false;
    bool draggingVolumeSlider_ = false;
    float mediaProgressSliderX_ = 0.0f;
    float mediaProgressSliderY_ = 0.0f;
    float mediaProgressSliderWidth_ = 0.0f;
    float mediaProgressSliderHeight_ = 4.0f;
    float mediaProgressSliderHitHeight_ = 24.0f;
    PrimitiveId mediaProgressSliderFillId_ = 0;
    PrimitiveId mediaProgressSliderKnobId_ = 0;
    PrimitiveId elapsedTimeTextId_ = 0;
    PrimitiveId totalTimeTextId_ = 0;
    PrimitiveId shuffleButtonId_ = 0;
    PrimitiveId playPauseButtonId_ = 0;
    float volumeSliderX_ = 0.0f;
    float volumeSliderY_ = 0.0f;
    float volumeSliderWidth_ = 0.0f;
    float volumeSliderHeight_ = 4.0f;
    float volumeSliderHitHeight_ = 24.0f;
    PrimitiveId volumeButtonId_ = 0;
    PrimitiveId volumeSliderFillId_ = 0;
    PrimitiveId volumeSliderKnobId_ = 0;
    PrimitiveId audioScanStatusTextId_ = 0;
    PrimitiveId audioScanPathTextId_ = 0;
    PrimitiveId audioScanProgressFillId_ = 0;
    PrimitiveId addSongsSearchFieldId_ = 0;
    bool primitivesDirty_ = false;
    VulkanRenderer::PrimitiveUpdate pendingPrimitiveUpdate_ = VulkanRenderer::PrimitiveUpdate::Full;
    bool sceneReady_ = false;
};

} // namespace womp
