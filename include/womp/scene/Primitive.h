#pragma once

#include <cstddef>
#include <cstdint>
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

struct TextFieldPrimitive {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
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
};

using PrimitiveGeometry = std::variant<
    RoundedRectPrimitive,
    CirclePrimitive,
    QuadPrimitive,
    TrianglePrimitive,
    LinePrimitive,
    TextPrimitive,
    TextFieldPrimitive>;

struct Primitive {
    PrimitiveId id = 0;
    PrimitiveGeometry geometry = RoundedRectPrimitive{};
    PrimitiveStyle style{};
    bool visible = true;

    static Primitive roundedRect(RoundedRectPrimitive geometry, PrimitiveStyle style = {});
    static Primitive circle(CirclePrimitive geometry, PrimitiveStyle style = {});
    static Primitive quad(QuadPrimitive geometry, PrimitiveStyle style = {});
    static Primitive triangle(TrianglePrimitive geometry, PrimitiveStyle style = {});
    static Primitive line(LinePrimitive geometry, PrimitiveStyle style = {});
    static Primitive text(TextPrimitive geometry, PrimitiveStyle style = {});
    static Primitive textField(TextFieldPrimitive geometry, PrimitiveStyle style = {});
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

    const std::vector<Primitive>& all() const;
    std::vector<Primitive> visible() const;

private:
    std::vector<Primitive>::iterator findIterator(PrimitiveId id);
    std::vector<Primitive>::const_iterator findIterator(PrimitiveId id) const;

    PrimitiveId nextId_ = 1;
    std::vector<Primitive> primitives_;
};

} // namespace womp
