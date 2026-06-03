#include "womp/App.h"

namespace womp {

App::App()
    : window_(1280, 720, "womp - Vulkan SDF")
    , renderer_(window_)
{
    renderer_.setSdfShapes({
        {
            .kind = VulkanRenderer::SdfShapeKind::RoundedRect,
            .x = 112.0f,
            .y = 96.0f,
            .width = 360.0f,
            .height = 180.0f,
            .radius = 28.0f,
            .strokeWidth = 2.0f,
            .fill = {0.95f, 0.97f, 1.0f, 0.92f},
            .stroke = {0.16f, 0.44f, 0.58f, 1.0f},
        },
        {
            .kind = VulkanRenderer::SdfShapeKind::Circle,
            .x = 560.0f,
            .y = 190.0f,
            .width = 72.0f,
            .height = 72.0f,
            .radius = 72.0f,
            .strokeWidth = 6.0f,
            .fill = {0.10f, 0.42f, 0.58f, 0.86f},
            .stroke = {1.0f, 0.82f, 0.32f, 0.95f},
        },
        {
            .kind = VulkanRenderer::SdfShapeKind::Line,
            .x = 126.0f,
            .y = 322.0f,
            .width = 650.0f,
            .height = 438.0f,
            .radius = 14.0f,
            .fill = {1.0f, 0.82f, 0.32f, 0.88f},
        },
    });
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
