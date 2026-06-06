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

Primitive Primitive::image(ImagePrimitive geometry, PrimitiveStyle style)
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

    indexesById_[primitive.id] = primitives_.size();
    primitives_.push_back(std::move(primitive));
    return primitives_.back().id;
}

bool PrimitiveStore::remove(PrimitiveId id)
{
    const auto found = indexesById_.find(id);
    if (found == indexesById_.end()) {
        return false;
    }

    const std::size_t index = found->second;
    primitives_.erase(primitives_.begin() + static_cast<std::ptrdiff_t>(index));
    indexesById_.erase(found);
    for (std::size_t shiftedIndex = index; shiftedIndex < primitives_.size(); ++shiftedIndex) {
        indexesById_[primitives_[shiftedIndex].id] = shiftedIndex;
    }
    return true;
}

void PrimitiveStore::clear()
{
    primitives_.clear();
    indexesById_.clear();
}

Primitive* PrimitiveStore::find(PrimitiveId id)
{
    const auto found = indexesById_.find(id);
    return found != indexesById_.end() ? &primitives_[found->second] : nullptr;
}

const Primitive* PrimitiveStore::find(PrimitiveId id) const
{
    const auto found = indexesById_.find(id);
    return found != indexesById_.end() ? &primitives_[found->second] : nullptr;
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

} // namespace womp
