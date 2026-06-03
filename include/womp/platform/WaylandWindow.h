#pragma once

#include <wayland-client.h>

#include <cstdint>
#include <string>

struct xdg_surface;
struct xdg_toplevel;
struct xdg_wm_base;

namespace womp {

class WaylandWindow {
public:
    WaylandWindow(std::uint32_t width, std::uint32_t height, const std::string& title);
    ~WaylandWindow();

    WaylandWindow(const WaylandWindow&) = delete;
    WaylandWindow& operator=(const WaylandWindow&) = delete;

    bool pollEvents();
    bool takeResizeFlag();

    wl_display* display() const { return display_; }
    wl_surface* surface() const { return surface_; }
    std::uint32_t width() const { return width_; }
    std::uint32_t height() const { return height_; }

    static void registryGlobal(
        void* data,
        wl_registry* registry,
        std::uint32_t name,
        const char* interface,
        std::uint32_t version);
    static void registryGlobalRemove(void* data, wl_registry* registry, std::uint32_t name);
    static void xdgPing(void* data, xdg_wm_base* shell, std::uint32_t serial);
    static void xdgSurfaceConfigure(void* data, xdg_surface* surface, std::uint32_t serial);
    static void xdgToplevelConfigure(
        void* data,
        xdg_toplevel* toplevel,
        std::int32_t width,
        std::int32_t height,
        wl_array* states);
    static void xdgToplevelClose(void* data, xdg_toplevel* toplevel);

private:
    void createShellSurface(const std::string& title);

    wl_display* display_ = nullptr;
    wl_registry* registry_ = nullptr;
    wl_compositor* compositor_ = nullptr;
    xdg_wm_base* shell_ = nullptr;
    wl_surface* surface_ = nullptr;
    xdg_surface* xdgSurface_ = nullptr;
    xdg_toplevel* toplevel_ = nullptr;

    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    bool configured_ = false;
    bool resized_ = false;
    bool shouldClose_ = false;
};

} // namespace womp
