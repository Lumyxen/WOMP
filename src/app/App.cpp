#include "womp/App.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <variant>

namespace womp {

namespace {

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

void buildInitialScene(PrimitiveStore& primitives, float windowHeight)
{
    constexpr float sidebarWidth = 280.0f;
    constexpr float sidebarPadding = 16.0f;
    constexpr float sidebarButtonHeight = 36.0f;
    constexpr float sidebarButtonGap = 4.0f;
    constexpr float sidebarButtonWidth = sidebarWidth - sidebarPadding * 2.0f;
    const Color sidebarBackground = rgb(45, 53, 59);
    const Color transparent = {0.0f, 0.0f, 0.0f, 0.0f};
    const Color sidebarText = rgb(211, 198, 170);
    const Color iconGrey = rgb(133, 146, 137);
    const Color accent = rgb(167, 192, 128);
    const Color border = rgb(71, 82, 88);
    const Color mantle = rgb(52, 63, 68);
    const Color activeFill = rgb(63, 74, 69);
    const Color separator = rgb(71, 82, 88);

    const std::string addSongsIcon = R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#859289" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="lucide lucide-plus-icon lucide-plus"><path d="M5 12h14"/><path d="M12 5v14"/></svg>)";
    const std::string settingsIcon = R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="none" stroke="#859289" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="lucide lucide-settings2-icon lucide-settings-2"><path d="M14 17H5"/><path d="M19 7h-9"/><circle cx="17" cy="17" r="3"/><circle cx="7" cy="7" r="3"/></svg>)";

    const auto addSidebarButton = [&](std::string label, std::string iconSvg, float y) {
        primitives.add(Primitive::button(
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
                .centerLabel = false,
            },
            {
                .fill = transparent,
                .stroke = transparent,
                .strokeWidth = 1.0f,
            }));
    };

    primitives.add(Primitive::roundedRect(
        {
            .x = 0.0f,
            .y = 0.0f,
            .width = sidebarWidth,
            .height = windowHeight,
            .radius = 0.0f,
        },
        panelStyle(sidebarBackground, 0.0f)));
    primitives.add(Primitive::line(
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
            .strokeWidth = 1.0f,
        }));

    const float helpY = windowHeight - sidebarPadding - sidebarButtonHeight;
    const float addSongsY = helpY - sidebarButtonGap - sidebarButtonHeight;
    primitives.add(Primitive::line(
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
            .strokeWidth = 1.0f,
        }));
    addSidebarButton("Add Songs", addSongsIcon, addSongsY);
    addSidebarButton("Settings", settingsIcon, helpY);
}

} // namespace

App::App()
    : window_(1280, 720, "womp")
    , renderer_(window_)
{
    buildInitialScene(primitives_, static_cast<float>(window_.height()));
    window_.setPointerEventHandler([this](const WaylandWindow::PointerEvent& event) {
        handlePointerEvent(event);
    });
    refreshPrimitives();
}

void App::run()
{
    while (window_.pollEvents()) {
        if (window_.takeResizeFlag()) {
            renderer_.recreateSwapchain();
            primitives_.clear();
            buildInitialScene(primitives_, static_cast<float>(window_.height()));
            pressedButton_ = 0;
            refreshPrimitives();
        }

        renderer_.drawFrame();
    }

    renderer_.waitIdle();
}

void App::handlePointerEvent(const WaylandWindow::PointerEvent& event)
{
    bool changed = false;
    bool hasHoveredButton = false;
    PrimitiveId releasedButton = 0;

    for (Primitive& primitive : primitives_.all()) {
        auto* button = std::get_if<ButtonPrimitive>(&primitive.geometry);
        if (button == nullptr) {
            continue;
        }

        const bool canInteract = primitive.visible && button->enabled;
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

    window_.setCursor(hasHoveredButton ? WaylandWindow::CursorShape::Pointer : WaylandWindow::CursorShape::Default);

    if (changed) {
        refreshPrimitives();
    }

    if (releasedButton != 0) {
        if (Primitive* primitive = primitives_.find(releasedButton)) {
            if (auto* button = std::get_if<ButtonPrimitive>(&primitive->geometry); button != nullptr && button->onClick) {
                button->onClick();
                refreshPrimitives();
            }
        }
    }
}

void App::refreshPrimitives()
{
    renderer_.setPrimitives(primitives_.visible());
}

} // namespace womp
