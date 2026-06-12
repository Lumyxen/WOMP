#pragma once

#include "womp/platform/WaylandWindow.h"
#include "womp/library/ImportService.h"
#include "womp/mpris/MprisService.h"
#include "womp/playback/AudioPlayer.h"
#include "womp/playback/PlaybackQueue.h"
#include "womp/playback/PlaybackStats.h"
#include "womp/renderer/VulkanRenderer.h"
#include "womp/scene/Primitive.h"

#include <array>
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
    App();

    void run();
    PlaylistId addPlaylist(std::string name);
    bool removePlaylist(PlaylistId id);

private:
    struct Playlist {
        PlaylistId id = 0;
        std::string name;
        std::string description;
        std::chrono::system_clock::time_point createdAt;
        std::int64_t position = 0;
        bool pinned = false;
        std::vector<TrackId> trackIndexes;
        std::unordered_set<TrackId> trackIndexSet;
    };

    struct Track {
        TrackId id;
        std::filesystem::path path;
        std::string title;
        std::string album;
        std::vector<std::string> artists;
        std::int64_t durationMs = 0;
        std::string formatLabel;
        std::filesystem::path artworkPath;
        bool available = true;
    };

    struct PlaylistTrackRowPrimitives {
        PrimitiveId background = 0;
        PrimitiveId coverTile = 0;
        PrimitiveId coverIcon = 0;
        PrimitiveId title = 0;
        PrimitiveId fileTypeBadgeBackground = 0;
        PrimitiveId fileTypeBadge = 0;
        PrimitiveId album = 0;
        PrimitiveId artist = 0;
        PrimitiveId duration = 0;
        PrimitiveId hoveredDuration = 0;
        PrimitiveId playButton = 0;
        PrimitiveId ellipsisButton = 0;
        bool active = false;
    };

    enum class DirectoryImportMode {
        Playlist,
        Files,
    };

    enum class TextFieldTarget {
        None,
        AddSongsSearch,
        PlaylistSearch,
        PlaylistTitle,
        PlaylistDescription,
    };

    struct TextEditState {
        std::size_t caretIndex = 0;
        std::size_t selectionAnchor = 0;
        float horizontalScrollOffset = 0.0f;
    };

    void selectPlaylist(PlaylistId id);
    void createPlaylist();
    bool setPlaylistPinned(PlaylistId id, bool pinned);
    void sortPlaylists();
    bool addTrackToPlaylist(PlaylistId playlistId, const TrackId& trackId);
    void toggleCreatePlaylistMenu();
    void closeCreatePlaylistMenu();
    void beginImportPlaylistFiles();
    void importPlaylistFiles(const std::vector<std::filesystem::path>& paths);
    void exportPlaylist(PlaylistId id);
    void beginPlaylistMetadataEdit(TextFieldTarget target);
    void finishPlaylistMetadataEdit(bool save);
    void toggleAddSongsMenu();
    void closeAddSongsMenu();
    void beginImportFiles();
    void completePendingAudioScanIfReady();
    void completePendingAudioImportIfReady();
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
    void resetSearchCaretBlink();
    void updateSearchCaretBlink();
    std::int32_t eventPollTimeoutMilliseconds(bool needsDraw) const;
    void togglePlayback();
    void toggleShuffle();
    void toggleTrackPlayback(const TrackId& trackId);
    void playTrack(const TrackId& trackId);
    void playSelectedSource();
    void playNext(bool userInitiated);
    void playPrevious();
    void startCurrentTrack();
    void pollPlayback();
    void flushPlaybackStats();
    void handleMprisCommands();
    void updateMpris();
    void openDeletePlaylistMenu(PlaylistId id);
    void closeDeletePlaylistMenu();
    void confirmPlaylistDeletion();
    bool selectedSourceIsCurrent() const;
    std::vector<TrackId> selectedSourceTrackIds() const;
    const Track* findTrack(const TrackId& id) const;
    Track* findTrack(const TrackId& id);
    void reloadLibrary();
    void toggleVolumeMute();
    void setVolumeFromPointer(float x);
    void setMediaProgressFromPointer(float x);
    void rebuildScene();
    void buildInitialScene(float windowWidth, float windowHeight);
    void handlePointerEvent(const WaylandWindow::PointerEvent& event);
    void handleKeyEvent(const WaylandWindow::KeyEvent& event);
    void clampTextEditState(TextEditState& state, const std::string& text) const;
    float textFieldScrollOffset(TextEditState& state, const std::string& text, float contentWidth, float fontSize) const;
    void refreshPrimitives(VulkanRenderer::PrimitiveUpdate update = VulkanRenderer::PrimitiveUpdate::DrawOnly);
    std::vector<Primitive> renderPrimitives(VulkanRenderer::PrimitiveUpdate update) const;
    bool anyModalOpen() const;
    bool activeModalContains(float x, float y) const;
    bool createPlaylistMenuContains(float x, float y) const;
    bool addSongsMenuContains(float x, float y) const;
    bool deletePlaylistMenuContains(float x, float y) const;
    bool addSongsSearchFieldContains(float x, float y) const;
    bool playlistSearchFieldContains(float x, float y) const;
    bool playlistTitleFieldContains(float x, float y) const;
    bool playlistDescriptionFieldContains(float x, float y) const;
    bool playlistMetadataFieldContains(PrimitiveId id, float x, float y) const;
    bool mediaProgressSliderContains(float x, float y) const;
    bool volumeSliderContains(float x, float y) const;
    bool canSeekMediaProgress() const;
    void refreshVolumeControl();
    void refreshMediaProgressControl();
    void resetVisualizerState();
    void applySpectrumFrame(const AudioPlayer::SpectrumFrame& frame);
    void refreshVisualizer();
    void refreshListenedTime();
    void setPlaylistSearchQuery(std::string query);
    void rebuildPlaylistTrackFilter();
    const Track* displayedPlaylistTrack(std::size_t displayedIndex) const;
    std::size_t displayedPlaylistTrackCount() const;
    std::string playlistTrackFileTypeBadge(const Track& track) const;
    std::size_t playlistTrackMaxFirstVisibleRow() const;
    void setPlaylistFirstVisibleRow(std::size_t firstVisibleRow);
    void refreshPlaylistTrackRows();
    void refreshPlaylistTrackRowHover(std::size_t hoveredSlot);
    void clearPlaylistTrackRowHover();
    bool playlistTrackViewportContains(float x, float y) const;
    std::size_t sidebarPlaylistMaxFirstVisibleRow() const;
    void setSidebarPlaylistFirstVisibleRow(std::size_t firstVisibleRow);
    bool sidebarPlaylistViewportContains(float x, float y) const;

    LibraryStore libraryStore_;
    ImportService importService_;
    AudioPlayer audioPlayer_;
    PlaybackQueue playbackQueue_;
    PlaybackStats playbackStats_;
    MprisService mprisService_;
    WaylandWindow window_;
    VulkanRenderer renderer_;
    PrimitiveStore primitives_;
    std::vector<Playlist> playlists_;
    std::vector<Track> tracks_;
    std::unordered_map<TrackId, std::size_t> trackIndexesById_;
    PlaylistId selectedPlaylistId_ = 0;
    PrimitiveId pressedButton_ = 0;
    PrimitiveId firstModalPrimitiveId_ = 0;
    bool createPlaylistMenuOpen_ = false;
    bool addSongsMenuOpen_ = false;
    PlaylistId deletePlaylistId_ = 0;
    bool addSongsSearchFocused_ = false;
    TextEditState addSongsSearchEdit_;
    bool addSongsDirectoryOptionsVisible_ = false;
    bool addSongsPlaylistDropdownOpen_ = false;
    bool searchCaretVisible_ = true;
    bool addSongsAudioScanActive_ = false;
    bool audioImportActive_ = false;
    bool playlistImportActive_ = false;
    std::chrono::steady_clock::time_point nextSearchCaretBlink_ = std::chrono::steady_clock::now();
    std::future<std::vector<PendingAudioFile>> pendingAudioScan_;
    std::future<ImportResult> pendingAudioImport_;
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
    std::size_t sidebarPlaylistFirstVisibleRow_ = 0;
    bool draggingSidebarPlaylistScrollbar_ = false;
    float sidebarPlaylistScrollbarDragOffsetY_ = 0.0f;
    float sidebarPlaylistViewportX_ = 0.0f;
    float sidebarPlaylistViewportY_ = 0.0f;
    float sidebarPlaylistViewportWidth_ = 0.0f;
    float sidebarPlaylistViewportHeight_ = 0.0f;
    float sidebarPlaylistScrollbarTrackX_ = 0.0f;
    float sidebarPlaylistScrollbarTrackY_ = 0.0f;
    float sidebarPlaylistScrollbarTrackWidth_ = 0.0f;
    float sidebarPlaylistScrollbarTrackHeight_ = 0.0f;
    float sidebarPlaylistScrollbarThumbY_ = 0.0f;
    float sidebarPlaylistScrollbarThumbHeight_ = 0.0f;
    bool playlistSearchFocused_ = false;
    TextEditState playlistSearchEdit_;
    std::string playlistSearchQuery_;
    std::string normalizedPlaylistSearchQuery_;
    TextFieldTarget playlistMetadataField_ = TextFieldTarget::None;
    TextEditState playlistTitleEdit_;
    TextEditState playlistDescriptionEdit_;
    std::string playlistTitleDraft_;
    std::string playlistDescriptionDraft_;
    std::vector<TrackId> filteredPlaylistTrackIndexes_;
    std::size_t playlistFirstVisibleRow_ = 0;
    std::size_t hoveredPlaylistTrackSlot_ = static_cast<std::size_t>(-1);
    TrackId lastClickedPlaylistTrackId_;
    std::uint32_t lastPlaylistTrackClickTimeMs_ = 0;
    bool draggingPlaylistScrollbar_ = false;
    float playlistScrollbarDragOffsetY_ = 0.0f;
    float playlistTrackViewportX_ = 0.0f;
    float playlistTrackViewportY_ = 0.0f;
    float playlistTrackViewportWidth_ = 0.0f;
    float playlistTrackViewportHeight_ = 0.0f;
    float playlistScrollbarTrackX_ = 0.0f;
    float playlistScrollbarTrackY_ = 0.0f;
    float playlistScrollbarTrackWidth_ = 0.0f;
    float playlistScrollbarTrackHeight_ = 0.0f;
    float playlistScrollbarThumbY_ = 0.0f;
    float playlistScrollbarThumbHeight_ = 0.0f;
    PrimitiveId playlistScrollbarTrackId_ = 0;
    PrimitiveId playlistScrollbarThumbId_ = 0;
    std::vector<PlaylistTrackRowPrimitives> playlistTrackRows_;
    bool draggingMediaProgressSlider_ = false;
    bool draggingVolumeSlider_ = false;
    float mediaProgressSliderX_ = 0.0f;
    float mediaProgressSliderY_ = 0.0f;
    float mediaProgressSliderWidth_ = 0.0f;
    float mediaProgressSliderHeight_ = 4.0f;
    float mediaProgressSliderHitHeight_ = 12.0f;
    PrimitiveId mediaProgressSliderFillId_ = 0;
    PrimitiveId mediaProgressSliderKnobId_ = 0;
    PrimitiveId elapsedTimeTextId_ = 0;
    PrimitiveId totalTimeTextId_ = 0;
    PrimitiveId listenedTimeTextId_ = 0;
    std::size_t listenedTimeMaxCharacters_ = 0;
    std::int64_t selectedPersistedListenedMs_ = 0;
    PrimitiveId shuffleButtonId_ = 0;
    PrimitiveId playPauseButtonId_ = 0;
    float volumeSliderX_ = 0.0f;
    float volumeSliderY_ = 0.0f;
    float volumeSliderWidth_ = 0.0f;
    float volumeSliderHeight_ = 4.0f;
    float volumeSliderHitHeight_ = 12.0f;
    PrimitiveId volumeButtonId_ = 0;
    PrimitiveId volumeSliderFillId_ = 0;
    PrimitiveId volumeSliderKnobId_ = 0;
    std::array<std::array<PrimitiveId, 10>, 2> visualizerBarIds_{};
    std::array<std::array<float, 10>, 2> visualizerLevels_{};
    std::array<float, 10> visualizerLowDb_{};
    std::array<float, 10> visualizerHighDb_{};
    bool visualizerRangeInitialized_ = false;
    float visualizerX_ = 0.0f;
    float visualizerY_ = 0.0f;
    float visualizerWidth_ = 0.0f;
    float visualizerHeight_ = 26.0f;
    bool visualizerVisible_ = false;
    PrimitiveId audioScanStatusTextId_ = 0;
    PrimitiveId audioScanPathTextId_ = 0;
    PrimitiveId audioScanProgressFillId_ = 0;
    PrimitiveId addSongsSearchFieldId_ = 0;
    PrimitiveId playlistSearchFieldId_ = 0;
    PrimitiveId playlistTitleFieldId_ = 0;
    PrimitiveId playlistDescriptionFieldId_ = 0;
    TextFieldTarget draggingSearchSelection_ = TextFieldTarget::None;
    bool primitivesDirty_ = false;
    VulkanRenderer::PrimitiveUpdate pendingPrimitiveUpdate_ = VulkanRenderer::PrimitiveUpdate::Full;
    bool sceneReady_ = false;
    std::chrono::steady_clock::time_point nextStatsFlush_ = std::chrono::steady_clock::now();
    std::string lastImportResultText_;
};

} // namespace womp
