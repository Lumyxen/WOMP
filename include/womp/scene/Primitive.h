#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace womp {

using PrimitiveId = std::uint64_t;

struct Color {
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;
};

struct PrimitiveStyle {
    Color fill{};
    Color stroke{};
    float strokeWidth = 0.0f;
};

struct PrimitiveClipRect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

struct RoundedRectPrimitive {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float radius = 0.0f;
};

struct CirclePrimitive {
    float centerX = 0.0f;
    float centerY = 0.0f;
    float radius = 0.0f;
};

struct QuadPrimitive {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

struct TrianglePrimitive {
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    float x2 = 0.0f;
    float y2 = 0.0f;
};

struct LinePrimitive {
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    float thickness = 1.0f;
};

struct TextPrimitive {
    float x = 0.0f;
    float y = 0.0f;
    float fontSize = 16.0f;
    std::string text;
    std::vector<std::string> fontFamilies;
};

enum class SvgSourceType {
    File,
    Data,
};

enum class SvgRenderMode {
    Color,
    Sdf,
    Mask,
};

struct SvgPrimitive {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float rasterScale = 1.0f;
    float sdfSpread = 8.0f;
    std::string source;
    SvgSourceType sourceType = SvgSourceType::File;
    SvgRenderMode renderMode = SvgRenderMode::Color;
};

struct TextFieldPrimitive {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float radius = 4.0f;
    float padding = 8.0f;
    float fontSize = 16.0f;
    float caretWidth = 1.0f;
    std::string text;
    std::string placeholder;
    std::vector<std::string> fontFamilies;
    Color textColor{0.05f, 0.05f, 0.05f, 1.0f};
    Color placeholderColor{0.45f, 0.45f, 0.45f, 1.0f};
    Color caretColor{0.05f, 0.05f, 0.05f, 1.0f};
    std::size_t caretCodepointIndex = 0;
    bool focused = false;
    bool caretVisible = true;
};

struct ButtonPrimitive {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float padding = 10.0f;
    float radius = 6.0f;
    float fontSize = 16.0f;
    float iconSize = 0.0f;
    float iconGap = 8.0f;
    std::string label;
    std::string iconSvg;
    std::vector<std::string> fontFamilies;
    Color iconColor{0.65f, 0.65f, 0.65f, 1.0f};
    Color labelColor{1.0f, 1.0f, 1.0f, 1.0f};
    Color hoverLabelColor{1.0f, 1.0f, 1.0f, 1.0f};
    Color pressedLabelColor{1.0f, 1.0f, 1.0f, 1.0f};
    Color disabledLabelColor{0.65f, 0.65f, 0.65f, 1.0f};
    Color hoverFill{0.22f, 0.22f, 0.22f, 1.0f};
    Color pressedFill{0.12f, 0.12f, 0.12f, 1.0f};
    Color disabledFill{0.24f, 0.24f, 0.24f, 1.0f};
    Color hoverStroke{0.0f, 0.0f, 0.0f, 0.0f};
    Color pressedStroke{0.0f, 0.0f, 0.0f, 0.0f};
    std::function<void()> onClick;
    bool hovered = false;
    bool pressed = false;
    bool enabled = true;
    bool centerLabel = true;
};

using PrimitiveGeometry = std::variant<
    RoundedRectPrimitive,
    CirclePrimitive,
    QuadPrimitive,
    TrianglePrimitive,
    LinePrimitive,
    TextPrimitive,
    SvgPrimitive,
    TextFieldPrimitive,
    ButtonPrimitive>;

struct Primitive {
    PrimitiveId id = 0;
    PrimitiveGeometry geometry = RoundedRectPrimitive{};
    PrimitiveStyle style{};
    bool visible = true;
    std::optional<PrimitiveClipRect> clip;

    static Primitive roundedRect(RoundedRectPrimitive geometry, PrimitiveStyle style = {});
    static Primitive circle(CirclePrimitive geometry, PrimitiveStyle style = {});
    static Primitive quad(QuadPrimitive geometry, PrimitiveStyle style = {});
    static Primitive triangle(TrianglePrimitive geometry, PrimitiveStyle style = {});
    static Primitive line(LinePrimitive geometry, PrimitiveStyle style = {});
    static Primitive text(TextPrimitive geometry, PrimitiveStyle style = {});
    static Primitive svg(SvgPrimitive geometry, PrimitiveStyle style = {});
    static Primitive textField(TextFieldPrimitive geometry, PrimitiveStyle style = {});
    static Primitive button(ButtonPrimitive geometry, PrimitiveStyle style = {});
};

class PrimitiveStore {
public:
    PrimitiveId add(Primitive primitive);
    bool remove(PrimitiveId id);
    void clear();

    Primitive* find(PrimitiveId id);
    const Primitive* find(PrimitiveId id) const;

    bool setVisible(PrimitiveId id, bool visible);
    bool empty() const;
    std::size_t size() const;

    std::vector<Primitive>& all();
    const std::vector<Primitive>& all() const;
    std::vector<Primitive> visible() const;

private:
    std::vector<Primitive>::iterator findIterator(PrimitiveId id);
    std::vector<Primitive>::const_iterator findIterator(PrimitiveId id) const;

    PrimitiveId nextId_ = 1;
    std::vector<Primitive> primitives_;
};

} // namespace womp
