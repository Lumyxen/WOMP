#pragma once

#include <wayland-client.h>
#include <wayland-cursor.h>

#include <cstdint>
#include <functional>
#include <string>

struct xdg_surface;
struct xdg_toplevel;
struct xdg_wm_base;

namespace womp {

class WaylandWindow {
public:
    enum class CursorShape {
        Default,
        Pointer,
    };

    enum class PointerEventType {
        Move,
        Leave,
        ButtonPress,
        ButtonRelease,
    };

    struct PointerEvent {
        PointerEventType type = PointerEventType::Move;
        float x = 0.0f;
        float y = 0.0f;
        std::uint32_t button = 0;
    };

    WaylandWindow(std::uint32_t width, std::uint32_t height, const std::string& title);
    ~WaylandWindow();

    WaylandWindow(const WaylandWindow&) = delete;
    WaylandWindow& operator=(const WaylandWindow&) = delete;

    bool pollEvents();
    bool takeResizeFlag();
    void setPointerEventHandler(std::function<void(const PointerEvent&)> handler);
    void setCursor(CursorShape shape);

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
    static void seatCapabilities(void* data, wl_seat* seat, std::uint32_t capabilities);
    static void seatName(void* data, wl_seat* seat, const char* name);
    static void pointerEnter(
        void* data,
        wl_pointer* pointer,
        std::uint32_t serial,
        wl_surface* surface,
        wl_fixed_t surfaceX,
        wl_fixed_t surfaceY);
    static void pointerLeave(void* data, wl_pointer* pointer, std::uint32_t serial, wl_surface* surface);
    static void pointerMotion(void* data, wl_pointer* pointer, std::uint32_t time, wl_fixed_t surfaceX, wl_fixed_t surfaceY);
    static void pointerButton(
        void* data,
        wl_pointer* pointer,
        std::uint32_t serial,
        std::uint32_t time,
        std::uint32_t button,
        std::uint32_t state);
    static void pointerAxis(void* data, wl_pointer* pointer, std::uint32_t time, std::uint32_t axis, wl_fixed_t value);
    static void pointerFrame(void* data, wl_pointer* pointer);
    static void pointerAxisSource(void* data, wl_pointer* pointer, std::uint32_t axisSource);
    static void pointerAxisStop(void* data, wl_pointer* pointer, std::uint32_t time, std::uint32_t axis);
    static void pointerAxisDiscrete(void* data, wl_pointer* pointer, std::uint32_t axis, std::int32_t discrete);

private:
    void createShellSurface(const std::string& title);
    void loadCursorTheme();
    wl_cursor* cursorForShape(CursorShape shape) const;
    void emitPointerEvent(PointerEvent event) const;

    wl_display* display_ = nullptr;
    wl_registry* registry_ = nullptr;
    wl_compositor* compositor_ = nullptr;
    wl_shm* shm_ = nullptr;
    wl_seat* seat_ = nullptr;
    wl_pointer* pointer_ = nullptr;
    wl_cursor_theme* cursorTheme_ = nullptr;
    wl_surface* cursorSurface_ = nullptr;
    xdg_wm_base* shell_ = nullptr;
    wl_surface* surface_ = nullptr;
    xdg_surface* xdgSurface_ = nullptr;
    xdg_toplevel* toplevel_ = nullptr;

    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    bool configured_ = false;
    bool resized_ = false;
    bool shouldClose_ = false;
    std::uint32_t pointerEnterSerial_ = 0;
    float pointerX_ = 0.0f;
    float pointerY_ = 0.0f;
    CursorShape cursorShape_ = CursorShape::Default;
    std::function<void(const PointerEvent&)> pointerEventHandler_;
};

} // namespace womp
