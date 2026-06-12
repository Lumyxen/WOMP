#pragma once

#include "womp/scene/Primitive.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace womp {

struct TextLayoutCharacter {
    std::uint32_t codepoint = 0;
    float advance = 0.0f;
};

struct TextLayoutGlyph {
    std::size_t index = 0;
    float x = 0.0f;
    float y = 0.0f;
    float advance = 0.0f;
};

struct TextLayoutLine {
    std::size_t startIndex = 0;
    std::size_t endIndex = 0;
    float y = 0.0f;
    float width = 0.0f;
    std::vector<float> caretXs;
};

struct TextLayoutRect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

struct TextLayout {
    std::vector<TextLayoutGlyph> glyphs;
    std::vector<TextLayoutLine> lines;
    float lineHeight = 0.0f;
    std::size_t characterCount = 0;
    bool overflow = false;

    std::size_t caretIndexAtPoint(float x, float y) const;
    std::size_t caretIndexOnAdjacentLine(std::size_t caretIndex, int direction) const;
    std::size_t visualLineStart(std::size_t caretIndex) const;
    std::size_t visualLineEnd(std::size_t caretIndex) const;
    TextLayoutRect caretRect(std::size_t caretIndex, float width = 0.0f) const;
    std::vector<TextLayoutRect> selectionRects(std::size_t startIndex, std::size_t endIndex) const;
};

TextLayout layoutText(
    const std::vector<TextLayoutCharacter>& characters,
    float contentWidth,
    TextWrapMode wrapMode,
    float lineHeight,
    std::size_t maxVisibleLines = std::numeric_limits<std::size_t>::max());

} // namespace womp
