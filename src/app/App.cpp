#include "womp/App.h"

namespace womp {

App::App()
    : window_(1280, 720, "womp - Vulkan background")
    , renderer_(window_)
{
}

void App::run()
{
    while (window_.pollEvents()) {
        if (window_.takeResizeFlag()) {
            renderer_.recreateSwapchain();
        }

        renderer_.drawFrame();
    }

    renderer_.waitIdle();
}

} // namespace womp
