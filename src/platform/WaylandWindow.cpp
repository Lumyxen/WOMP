#include "womp/platform/WaylandWindow.h"

#include "xdg-shell-client-protocol.h"

#include <poll.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

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

bool WaylandWindow::pollEvents()
{
    wl_display_dispatch_pending(display_);

    while (wl_display_prepare_read(display_) != 0) {
        wl_display_dispatch_pending(display_);
    }

    wl_display_flush(display_);

    pollfd descriptor{
        .fd = wl_display_get_fd(display_),
        .events = POLLIN,
        .revents = 0,
    };

    if (poll(&descriptor, 1, 0) > 0 && (descriptor.revents & POLLIN) != 0) {
        if (wl_display_read_events(display_) == -1) {
            return false;
        }
    } else {
        wl_display_cancel_read(display_);
    }

    wl_display_dispatch_pending(display_);
    return !shouldClose_;
}

bool WaylandWindow::takeResizeFlag()
{
    const bool wasResized = resized_;
    resized_ = false;
    return wasResized;
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

    if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
        window->shell_ = static_cast<xdg_wm_base*>(
            wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
        xdg_wm_base_add_listener(window->shell_, &shellListener, window);
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

    // Zero means the compositor leaves this dimension to the client.
    if (width <= 0 || height <= 0) {
        return;
    }

    const auto newWidth = static_cast<std::uint32_t>(width);
    const auto newHeight = static_cast<std::uint32_t>(height);
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

} // namespace womp
