#pragma once

#include "womp/platform/WaylandWindow.h"
#include "womp/renderer/VulkanRenderer.h"
#include "womp/scene/Primitive.h"

#include <cstdint>
#include <string>
#include <vector>

namespace womp {

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
    };

    void selectPlaylist(PlaylistId id);
    void rebuildScene();
    void buildInitialScene(float windowHeight);
    void handlePointerEvent(const WaylandWindow::PointerEvent& event);
    void refreshPrimitives();

    WaylandWindow window_;
    VulkanRenderer renderer_;
    PrimitiveStore primitives_;
    std::vector<Playlist> playlists_;
    PlaylistId nextPlaylistId_ = 1;
    PlaylistId selectedPlaylistId_ = 0;
    PrimitiveId pressedButton_ = 0;
    bool sceneReady_ = false;
};

} // namespace womp
