#include "womp/scene/Primitive.h"

#include <algorithm>
#include <utility>

namespace womp {

Primitive Primitive::roundedRect(RoundedRectPrimitive geometry, PrimitiveStyle style)
{
    return {
        .geometry = geometry,
        .style = style,
    };
}

Primitive Primitive::circle(CirclePrimitive geometry, PrimitiveStyle style)
{
    return {
        .geometry = geometry,
        .style = style,
    };
}

Primitive Primitive::quad(QuadPrimitive geometry, PrimitiveStyle style)
{
    return {
        .geometry = geometry,
        .style = style,
    };
}

Primitive Primitive::triangle(TrianglePrimitive geometry, PrimitiveStyle style)
{
    return {
        .geometry = geometry,
        .style = style,
    };
}

Primitive Primitive::line(LinePrimitive geometry, PrimitiveStyle style)
{
    return {
        .geometry = geometry,
        .style = style,
    };
}

Primitive Primitive::text(TextPrimitive geometry, PrimitiveStyle style)
{
    return {
        .geometry = std::move(geometry),
        .style = style,
    };
}

Primitive Primitive::svg(SvgPrimitive geometry, PrimitiveStyle style)
{
    return {
        .geometry = std::move(geometry),
        .style = style,
    };
}

Primitive Primitive::textField(TextFieldPrimitive geometry, PrimitiveStyle style)
{
    return {
        .geometry = std::move(geometry),
        .style = style,
    };
}

Primitive Primitive::button(ButtonPrimitive geometry, PrimitiveStyle style)
{
    return {
        .geometry = std::move(geometry),
        .style = style,
    };
}

PrimitiveId PrimitiveStore::add(Primitive primitive)
{
    if (primitive.id == 0) {
        primitive.id = nextId_++;
    } else {
        nextId_ = std::max(nextId_, primitive.id + 1);
    }

    primitives_.push_back(primitive);
    return primitives_.back().id;
}

bool PrimitiveStore::remove(PrimitiveId id)
{
    const auto primitive = findIterator(id);
    if (primitive == primitives_.end()) {
        return false;
    }

    primitives_.erase(primitive);
    return true;
}

void PrimitiveStore::clear()
{
    primitives_.clear();
}

Primitive* PrimitiveStore::find(PrimitiveId id)
{
    const auto primitive = findIterator(id);
    return primitive != primitives_.end() ? &*primitive : nullptr;
}

const Primitive* PrimitiveStore::find(PrimitiveId id) const
{
    const auto primitive = findIterator(id);
    return primitive != primitives_.end() ? &*primitive : nullptr;
}

bool PrimitiveStore::setVisible(PrimitiveId id, bool visible)
{
    Primitive* primitive = find(id);
    if (primitive == nullptr) {
        return false;
    }

    primitive->visible = visible;
    return true;
}

bool PrimitiveStore::empty() const
{
    return primitives_.empty();
}

std::size_t PrimitiveStore::size() const
{
    return primitives_.size();
}

std::vector<Primitive>& PrimitiveStore::all()
{
    return primitives_;
}

const std::vector<Primitive>& PrimitiveStore::all() const
{
    return primitives_;
}

std::vector<Primitive> PrimitiveStore::visible() const
{
    std::vector<Primitive> result;
    result.reserve(primitives_.size());

    for (const Primitive& primitive : primitives_) {
        if (primitive.visible) {
            result.push_back(primitive);
        }
    }

    return result;
}

std::vector<Primitive>::iterator PrimitiveStore::findIterator(PrimitiveId id)
{
    return std::ranges::find(primitives_, id, &Primitive::id);
}

std::vector<Primitive>::const_iterator PrimitiveStore::findIterator(PrimitiveId id) const
{
    return std::ranges::find(primitives_, id, &Primitive::id);
}

} // namespace womp
