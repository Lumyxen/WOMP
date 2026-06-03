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
    void handlePointerEvent(const WaylandWindow::PointerEvent& event);
    void refreshPrimitives();

    WaylandWindow window_;
    VulkanRenderer renderer_;
    PrimitiveStore primitives_;
    PrimitiveId pressedButton_ = 0;
};

} // namespace womp
