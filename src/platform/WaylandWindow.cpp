#include "womp/platform/WaylandWindow.h"

#include "xdg-shell-client-protocol.h"

#include <poll.h>

#include <linux/input-event-codes.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <unistd.h>
#include <utility>

namespace womp {

namespace {

constexpr wl_registry_listener registryListener{
    .global = WaylandWindow::registryGlobal,
    .global_remove = WaylandWindow::registryGlobalRemove,
};

constexpr xdg_wm_base_listener shellListener{
    .ping = WaylandWindow::xdgPing,
};

constexpr xdg_surface_listener surfaceListener{
    .configure = WaylandWindow::xdgSurfaceConfigure,
};

constexpr xdg_toplevel_listener toplevelListener{
    .configure = WaylandWindow::xdgToplevelConfigure,
    .close = WaylandWindow::xdgToplevelClose,
};

constexpr wl_seat_listener seatListener{
    .capabilities = WaylandWindow::seatCapabilities,
    .name = WaylandWindow::seatName,
};

constexpr wl_pointer_listener pointerListener{
    .enter = WaylandWindow::pointerEnter,
    .leave = WaylandWindow::pointerLeave,
    .motion = WaylandWindow::pointerMotion,
    .button = WaylandWindow::pointerButton,
    .axis = WaylandWindow::pointerAxis,
    .frame = WaylandWindow::pointerFrame,
    .axis_source = WaylandWindow::pointerAxisSource,
    .axis_stop = WaylandWindow::pointerAxisStop,
    .axis_discrete = WaylandWindow::pointerAxisDiscrete,
};

constexpr wl_keyboard_listener keyboardListener{
    .keymap = WaylandWindow::keyboardKeymap,
    .enter = WaylandWindow::keyboardEnter,
    .leave = WaylandWindow::keyboardLeave,
    .key = WaylandWindow::keyboardKey,
    .modifiers = WaylandWindow::keyboardModifiers,
    .repeat_info = WaylandWindow::keyboardRepeatInfo,
};

} // namespace

WaylandWindow::WaylandWindow(std::uint32_t width, std::uint32_t height, const std::string& title)
    : width_(width)
    , height_(height)
{
    display_ = wl_display_connect(nullptr);
    if (display_ == nullptr) {
        throw std::runtime_error("failed to connect to a Wayland display");
    }

    registry_ = wl_display_get_registry(display_);
    wl_registry_add_listener(registry_, &registryListener, this);

    wl_display_roundtrip(display_);
    if (compositor_ == nullptr || shell_ == nullptr) {
        throw std::runtime_error("Wayland compositor does not expose wl_compositor and xdg_wm_base");
    }

    createShellSurface(title);

    // Wait for the first configure before rendering into the surface.
    while (!configured_ && wl_display_dispatch(display_) != -1) {
    }

    if (!configured_) {
        throw std::runtime_error("Wayland surface was not configured");
    }
}

WaylandWindow::~WaylandWindow()
{
    if (toplevel_ != nullptr) {
        xdg_toplevel_destroy(toplevel_);
    }
    if (xdgSurface_ != nullptr) {
        xdg_surface_destroy(xdgSurface_);
    }
    if (surface_ != nullptr) {
        wl_surface_destroy(surface_);
    }
    if (shell_ != nullptr) {
        xdg_wm_base_destroy(shell_);
    }
    if (pointer_ != nullptr) {
        wl_pointer_destroy(pointer_);
    }
    if (keyboard_ != nullptr) {
        wl_keyboard_destroy(keyboard_);
    }
    if (cursorSurface_ != nullptr) {
        wl_surface_destroy(cursorSurface_);
    }
    if (cursorTheme_ != nullptr) {
        wl_cursor_theme_destroy(cursorTheme_);
    }
    if (seat_ != nullptr) {
        wl_seat_destroy(seat_);
    }
    if (shm_ != nullptr) {
        wl_shm_destroy(shm_);
    }
    if (compositor_ != nullptr) {
        wl_compositor_destroy(compositor_);
    }
    if (registry_ != nullptr) {
        wl_registry_destroy(registry_);
    }
    if (display_ != nullptr) {
        wl_display_disconnect(display_);
    }
}

bool WaylandWindow::pollEvents(std::int32_t timeoutMilliseconds, int wakeFd)
{
    const auto dispatchPending = [this]() {
        const int dispatched = wl_display_dispatch_pending(display_);
        return dispatched >= 0 ? dispatched : -1;
    };

    int dispatched = dispatchPending();
    if (dispatched < 0) {
        return false;
    }
    if (dispatched > 0) {
        wl_display_flush(display_);
        return !shouldClose_;
    }

    while (wl_display_prepare_read(display_) != 0) {
        dispatched = dispatchPending();
        if (dispatched < 0) {
            return false;
        }
        if (dispatched > 0) {
            wl_display_flush(display_);
            return !shouldClose_;
        }
    }

    wl_display_flush(display_);

    pollfd descriptors[2]{
        {.fd = wl_display_get_fd(display_), .events = POLLIN, .revents = 0},
        {.fd = wakeFd, .events = POLLIN, .revents = 0},
    };
    const nfds_t descriptorCount = wakeFd >= 0 ? 2 : 1;

    if (poll(descriptors, descriptorCount, timeoutMilliseconds) > 0 && (descriptors[0].revents & POLLIN) != 0) {
        if (wl_display_read_events(display_) == -1) {
            return false;
        }
    } else {
        wl_display_cancel_read(display_);
    }

    if (dispatchPending() < 0) {
        return false;
    }
    return !shouldClose_;
}

bool WaylandWindow::takeResizeFlag()
{
    const bool wasResized = resized_;
    resized_ = false;
    return wasResized;
}

void WaylandWindow::setPointerEventHandler(std::function<void(const PointerEvent&)> handler)
{
    pointerEventHandler_ = std::move(handler);
}

void WaylandWindow::setKeyEventHandler(std::function<void(const KeyEvent&)> handler)
{
    keyEventHandler_ = std::move(handler);
}

void WaylandWindow::setCursor(CursorShape shape)
{
    if (shape == cursorShape_ && cursorSerial_ == pointerEnterSerial_) {
        return;
    }

    cursorShape_ = shape;

    if (pointer_ == nullptr || cursorSurface_ == nullptr || pointerEnterSerial_ == 0) {
        return;
    }

    wl_cursor* cursor = cursorForShape(shape);
    if (cursor == nullptr || cursor->image_count == 0) {
        return;
    }

    wl_cursor_image* image = cursor->images[0];
    wl_buffer* buffer = wl_cursor_image_get_buffer(image);
    if (buffer == nullptr) {
        return;
    }

    wl_pointer_set_cursor(
        pointer_,
        pointerEnterSerial_,
        cursorSurface_,
        static_cast<std::int32_t>(image->hotspot_x),
        static_cast<std::int32_t>(image->hotspot_y));
    wl_surface_attach(cursorSurface_, buffer, 0, 0);
    wl_surface_damage_buffer(cursorSurface_, 0, 0, static_cast<std::int32_t>(image->width), static_cast<std::int32_t>(image->height));
    wl_surface_commit(cursorSurface_);
    cursorSerial_ = pointerEnterSerial_;
}

void WaylandWindow::registryGlobal(
    void* data,
    wl_registry* registry,
    std::uint32_t name,
    const char* interface,
    std::uint32_t version)
{
    auto* window = static_cast<WaylandWindow*>(data);

    if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
        const std::uint32_t bindVersion = std::min(version, 4u);
        window->compositor_ = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, bindVersion));
    }

    if (std::strcmp(interface, wl_shm_interface.name) == 0) {
        window->shm_ = static_cast<wl_shm*>(
            wl_registry_bind(registry, name, &wl_shm_interface, 1));
        window->loadCursorTheme();
    }

    if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
        window->shell_ = static_cast<xdg_wm_base*>(
            wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
        xdg_wm_base_add_listener(window->shell_, &shellListener, window);
    }

    if (std::strcmp(interface, wl_seat_interface.name) == 0) {
        const std::uint32_t bindVersion = std::min(version, 5u);
        window->seat_ = static_cast<wl_seat*>(
            wl_registry_bind(registry, name, &wl_seat_interface, bindVersion));
        wl_seat_add_listener(window->seat_, &seatListener, window);
    }
}

void WaylandWindow::registryGlobalRemove(void*, wl_registry*, std::uint32_t)
{
}

void WaylandWindow::xdgPing(void*, xdg_wm_base* shell, std::uint32_t serial)
{
    xdg_wm_base_pong(shell, serial);
}

void WaylandWindow::xdgSurfaceConfigure(void* data, xdg_surface* surface, std::uint32_t serial)
{
    auto* window = static_cast<WaylandWindow*>(data);
    xdg_surface_ack_configure(surface, serial);
    window->configured_ = true;
}

void WaylandWindow::xdgToplevelConfigure(
    void* data,
    xdg_toplevel*,
    std::int32_t width,
    std::int32_t height,
    wl_array*)
{
    auto* window = static_cast<WaylandWindow*>(data);

    // Zero means the compositor leaves that dimension to the client.
    const auto newWidth = width > 0 ? static_cast<std::uint32_t>(width) : window->width_;
    const auto newHeight = height > 0 ? static_cast<std::uint32_t>(height) : window->height_;
    if (newWidth != window->width_ || newHeight != window->height_) {
        window->width_ = newWidth;
        window->height_ = newHeight;
        window->resized_ = true;
    }
}

void WaylandWindow::xdgToplevelClose(void* data, xdg_toplevel*)
{
    static_cast<WaylandWindow*>(data)->shouldClose_ = true;
}

void WaylandWindow::seatCapabilities(void* data, wl_seat* seat, std::uint32_t capabilities)
{
    auto* window = static_cast<WaylandWindow*>(data);
    const bool hasPointer = (capabilities & WL_SEAT_CAPABILITY_POINTER) != 0;
    const bool hasKeyboard = (capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0;

    if (hasPointer && window->pointer_ == nullptr) {
        window->pointer_ = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(window->pointer_, &pointerListener, window);
        window->loadCursorTheme();
    } else if (!hasPointer && window->pointer_ != nullptr) {
        wl_pointer_destroy(window->pointer_);
        window->pointer_ = nullptr;
        window->emitPointerEvent({.type = PointerEventType::Leave, .x = window->pointerX_, .y = window->pointerY_});
    }

    if (hasKeyboard && window->keyboard_ == nullptr) {
        window->keyboard_ = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(window->keyboard_, &keyboardListener, window);
    } else if (!hasKeyboard && window->keyboard_ != nullptr) {
        wl_keyboard_destroy(window->keyboard_);
        window->keyboard_ = nullptr;
    }
}

void WaylandWindow::seatName(void*, wl_seat*, const char*)
{
}

void WaylandWindow::pointerEnter(
    void* data,
    wl_pointer*,
    std::uint32_t serial,
    wl_surface*,
    wl_fixed_t surfaceX,
    wl_fixed_t surfaceY)
{
    auto* window = static_cast<WaylandWindow*>(data);
    window->pointerEnterSerial_ = serial;
    window->pointerX_ = static_cast<float>(wl_fixed_to_double(surfaceX));
    window->pointerY_ = static_cast<float>(wl_fixed_to_double(surfaceY));
    window->setCursor(window->cursorShape_);
    window->emitPointerEvent({.type = PointerEventType::Move, .x = window->pointerX_, .y = window->pointerY_});
}

void WaylandWindow::pointerLeave(void* data, wl_pointer*, std::uint32_t, wl_surface*)
{
    auto* window = static_cast<WaylandWindow*>(data);
    window->pointerEnterSerial_ = 0;
    window->cursorSerial_ = 0;
    window->emitPointerEvent({.type = PointerEventType::Leave, .x = window->pointerX_, .y = window->pointerY_});
}

void WaylandWindow::pointerMotion(void* data, wl_pointer*, std::uint32_t, wl_fixed_t surfaceX, wl_fixed_t surfaceY)
{
    auto* window = static_cast<WaylandWindow*>(data);
    window->pointerX_ = static_cast<float>(wl_fixed_to_double(surfaceX));
    window->pointerY_ = static_cast<float>(wl_fixed_to_double(surfaceY));
    window->emitPointerEvent({.type = PointerEventType::Move, .x = window->pointerX_, .y = window->pointerY_});
}

void WaylandWindow::pointerButton(
    void* data,
    wl_pointer*,
    std::uint32_t,
    std::uint32_t time,
    std::uint32_t button,
    std::uint32_t state)
{
    auto* window = static_cast<WaylandWindow*>(data);
    if (button != BTN_LEFT) {
        return;
    }

    window->emitPointerEvent({
        .type = state == WL_POINTER_BUTTON_STATE_PRESSED ? PointerEventType::ButtonPress : PointerEventType::ButtonRelease,
        .x = window->pointerX_,
        .y = window->pointerY_,
        .button = button,
        .timeMs = time,
    });
}

void WaylandWindow::pointerAxis(void* data, wl_pointer*, std::uint32_t, std::uint32_t axis, wl_fixed_t value)
{
    auto* window = static_cast<WaylandWindow*>(data);
    if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
        return;
    }

    window->emitPointerEvent({
        .type = PointerEventType::Scroll,
        .x = window->pointerX_,
        .y = window->pointerY_,
        .scrollY = static_cast<float>(wl_fixed_to_double(value)),
    });
}

void WaylandWindow::pointerFrame(void*, wl_pointer*)
{
}

void WaylandWindow::pointerAxisSource(void*, wl_pointer*, std::uint32_t)
{
}

void WaylandWindow::pointerAxisStop(void*, wl_pointer*, std::uint32_t, std::uint32_t)
{
}

void WaylandWindow::pointerAxisDiscrete(void*, wl_pointer*, std::uint32_t, std::int32_t)
{
}

void WaylandWindow::keyboardKeymap(void*, wl_keyboard*, std::uint32_t, std::int32_t fd, std::uint32_t)
{
    close(fd);
}

void WaylandWindow::keyboardEnter(void*, wl_keyboard*, std::uint32_t, wl_surface*, wl_array*)
{
}

void WaylandWindow::keyboardLeave(void* data, wl_keyboard*, std::uint32_t, wl_surface*)
{
    auto* window = static_cast<WaylandWindow*>(data);
    window->controlPressed_ = false;
    window->shiftPressed_ = false;
    window->altPressed_ = false;
}

void WaylandWindow::keyboardKey(
    void* data,
    wl_keyboard*,
    std::uint32_t,
    std::uint32_t,
    std::uint32_t key,
    std::uint32_t state)
{
    auto* window = static_cast<WaylandWindow*>(data);
    const bool pressed = state == WL_KEYBOARD_KEY_STATE_PRESSED;
    switch (key) {
    case KEY_LEFTCTRL:
    case KEY_RIGHTCTRL:
        window->controlPressed_ = pressed;
        break;
    case KEY_LEFTSHIFT:
    case KEY_RIGHTSHIFT:
        window->shiftPressed_ = pressed;
        break;
    case KEY_LEFTALT:
    case KEY_RIGHTALT:
        window->altPressed_ = pressed;
        break;
    default:
        break;
    }

    if (window->keyEventHandler_) {
        window->keyEventHandler_({
            .type = pressed ? KeyEventType::Press : KeyEventType::Release,
            .key = key,
            .control = window->controlPressed_,
            .shift = window->shiftPressed_,
            .alt = window->altPressed_,
        });
    }
}

void WaylandWindow::keyboardModifiers(void*, wl_keyboard*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t)
{
}

void WaylandWindow::keyboardRepeatInfo(void*, wl_keyboard*, std::int32_t, std::int32_t)
{
}

void WaylandWindow::emitPointerEvent(PointerEvent event) const
{
    if (pointerEventHandler_) {
        pointerEventHandler_(event);
    }
}

void WaylandWindow::createShellSurface(const std::string& title)
{
    surface_ = wl_compositor_create_surface(compositor_);
    if (surface_ == nullptr) {
        throw std::runtime_error("failed to create a Wayland surface");
    }

    xdgSurface_ = xdg_wm_base_get_xdg_surface(shell_, surface_);
    xdg_surface_add_listener(xdgSurface_, &surfaceListener, this);

    toplevel_ = xdg_surface_get_toplevel(xdgSurface_);
    xdg_toplevel_add_listener(toplevel_, &toplevelListener, this);
    xdg_toplevel_set_title(toplevel_, title.c_str());

    wl_surface_commit(surface_);
}

void WaylandWindow::loadCursorTheme()
{
    if (cursorTheme_ == nullptr) {
        if (shm_ == nullptr) {
            return;
        }

        cursorTheme_ = wl_cursor_theme_load(nullptr, 24, shm_);
    }

    if (cursorSurface_ == nullptr && compositor_ != nullptr) {
        cursorSurface_ = wl_compositor_create_surface(compositor_);
    }
}

wl_cursor* WaylandWindow::cursorForShape(CursorShape shape) const
{
    if (cursorTheme_ == nullptr) {
        return nullptr;
    }

    switch (shape) {
    case CursorShape::Text:
        if (wl_cursor* cursor = wl_cursor_theme_get_cursor(cursorTheme_, "text")) {
            return cursor;
        }
        return wl_cursor_theme_get_cursor(cursorTheme_, "xterm");
    case CursorShape::Pointer:
        if (wl_cursor* cursor = wl_cursor_theme_get_cursor(cursorTheme_, "pointer")) {
            return cursor;
        }
        return wl_cursor_theme_get_cursor(cursorTheme_, "hand1");
    case CursorShape::Default:
        if (wl_cursor* cursor = wl_cursor_theme_get_cursor(cursorTheme_, "default")) {
            return cursor;
        }
        return wl_cursor_theme_get_cursor(cursorTheme_, "left_ptr");
    }

    return nullptr;
}

} // namespace womp
