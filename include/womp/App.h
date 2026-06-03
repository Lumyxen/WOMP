#pragma once

#include "womp/platform/WaylandWindow.h"
#include "womp/renderer/VulkanRenderer.h"

namespace womp {

class App {
public:
    App();

    void run();

private:
    WaylandWindow window_;
    VulkanRenderer renderer_;
};

} // namespace womp
