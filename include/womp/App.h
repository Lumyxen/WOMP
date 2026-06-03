#pragma once

#include "womp/platform/WaylandWindow.h"
#include "womp/renderer/VulkanRenderer.h"
#include "womp/scene/Primitive.h"

namespace womp {

class App {
public:
    App();

    void run();

private:
    WaylandWindow window_;
    VulkanRenderer renderer_;
    PrimitiveStore primitives_;
};

} // namespace womp
