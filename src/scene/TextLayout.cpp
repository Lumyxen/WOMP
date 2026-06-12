#include "womp/scene/TextLayout.h"

#include <algorithm>
#include <cmath>

namespace womp {
namespace {

bool isWrapSpace(std::uint32_t codepoint)
{
    return codepoint == ' ' || codepoint == '\t';
}

const TextLayoutLine& lineForCaret(const TextLayout& layout, std::size_t caretIndex)
{
    const std::size_t clamped = std::min(caretIndex, layout.characterCount);
    for (std::size_t index = 0; index < layout.lines.size(); ++index) {
        const TextLayoutLine& line = layout.lines[index];
        const bool explicitLineBreak = index + 1 < layout.lines.size()
            && layout.lines[index + 1].startIndex > line.endIndex;
        if (clamped >= line.startIndex
            && (clamped < line.endIndex
                || (clamped == line.endIndex && (index + 1 == layout.lines.size() || explicitLineBreak)))) {
            return line;
        }
    }
    return layout.lines.back();
}

std::size_t lineIndexForCaret(const TextLayout& layout, std::size_t caretIndex)
{
    const TextLayoutLine* line = &lineForCaret(layout, caretIndex);
    return static_cast<std::size_t>(line - layout.lines.data());
}

float caretX(const TextLayoutLine& line, std::size_t index)
{
    const std::size_t offset = std::clamp(index, line.startIndex, line.endIndex) - line.startIndex;
    return line.caretXs[offset];
}

} // namespace

TextLayout layoutText(
    const std::vector<TextLayoutCharacter>& characters,
    float contentWidth,
    TextWrapMode wrapMode,
    float lineHeight,
    std::size_t maxVisibleLines)
{
    TextLayout result{
        .lineHeight = lineHeight,
        .characterCount = characters.size(),
    };
    const float availableWidth = std::max(0.0f, contentWidth);
    const std::size_t visibleLimit = std::max<std::size_t>(1, maxVisibleLines);

    const auto addLine = [&](std::size_t start, std::size_t end) {
        if (result.lines.size() >= visibleLimit) {
            result.overflow = true;
            return;
        }
        TextLayoutLine line{
            .startIndex = start,
            .endIndex = end,
            .y = static_cast<float>(result.lines.size()) * lineHeight,
        };
        line.caretXs.reserve(end - start + 1);
        line.caretXs.push_back(0.0f);
        for (std::size_t index = start; index < end; ++index) {
            const float advance = std::max(0.0f, characters[index].advance);
            result.glyphs.push_back({
                .index = index,
                .x = line.width,
                .y = line.y,
                .advance = advance,
            });
            line.width += advance;
            line.caretXs.push_back(line.width);
        }
        result.lines.push_back(std::move(line));
    };

    std::size_t paragraphStart = 0;
    while (paragraphStart <= characters.size()) {
        std::size_t paragraphEnd = paragraphStart;
        while (paragraphEnd < characters.size() && characters[paragraphEnd].codepoint != '\n') {
            ++paragraphEnd;
        }

        std::size_t lineStart = paragraphStart;
        do {
            std::size_t fitEnd = lineStart;
            float width = 0.0f;
            while (fitEnd < paragraphEnd) {
                const float advance = std::max(0.0f, characters[fitEnd].advance);
                if (wrapMode == TextWrapMode::Word
                    && fitEnd > lineStart
                    && width + advance > availableWidth) {
                    break;
                }
                width += advance;
                ++fitEnd;
                if (wrapMode == TextWrapMode::Word && width > availableWidth) {
                    break;
                }
            }

            std::size_t lineEnd = paragraphEnd;
            std::size_t nextLineStart = paragraphEnd;
            if (wrapMode == TextWrapMode::Word && fitEnd < paragraphEnd) {
                lineEnd = fitEnd;
                if (fitEnd > lineStart && isWrapSpace(characters[fitEnd].codepoint)) {
                    nextLineStart = fitEnd + 1;
                } else {
                    for (std::size_t index = fitEnd; index > lineStart; --index) {
                        const std::size_t separator = index - 1;
                        if (separator > lineStart && isWrapSpace(characters[separator].codepoint)) {
                            lineEnd = separator;
                            nextLineStart = separator + 1;
                            break;
                        }
                    }
                }
                if (lineEnd == lineStart) {
                    lineEnd = std::min(paragraphEnd, lineStart + 1);
                }
                if (nextLineStart == paragraphEnd) {
                    nextLineStart = lineEnd;
                }
            }

            addLine(lineStart, lineEnd);
            lineStart = nextLineStart;
        } while (lineStart < paragraphEnd);

        if (paragraphEnd == characters.size()) {
            break;
        }
        paragraphStart = paragraphEnd + 1;
        if (result.lines.size() >= visibleLimit && paragraphStart <= characters.size()) {
            result.overflow = true;
        }
    }

    if (result.lines.empty()) {
        addLine(0, 0);
    }
    return result;
}

std::size_t TextLayout::caretIndexAtPoint(float x, float y) const
{
    if (lines.empty()) {
        return 0;
    }
    const std::size_t lineIndex = std::clamp<std::size_t>(
        lineHeight > 0.0f && y > 0.0f ? static_cast<std::size_t>(std::floor(y / lineHeight)) : 0,
        0,
        lines.size() - 1);
    const TextLayoutLine& line = lines[lineIndex];
    for (std::size_t offset = 1; offset < line.caretXs.size(); ++offset) {
        if (x < (line.caretXs[offset - 1] + line.caretXs[offset]) * 0.5f) {
            return line.startIndex + offset - 1;
        }
    }
    return line.endIndex;
}

std::size_t TextLayout::caretIndexOnAdjacentLine(std::size_t caretIndex, int direction) const
{
    if (lines.empty() || direction == 0) {
        return std::min(caretIndex, characterCount);
    }
    const std::size_t currentLineIndex = lineIndexForCaret(*this, caretIndex);
    if ((direction < 0 && currentLineIndex == 0)
        || (direction > 0 && currentLineIndex + 1 >= lines.size())) {
        return std::min(caretIndex, characterCount);
    }
    const TextLayoutLine& currentLine = lines[currentLineIndex];
    const TextLayoutLine& targetLine = lines[
        direction < 0 ? currentLineIndex - 1 : currentLineIndex + 1];
    const float targetX = caretX(currentLine, caretIndex);
    for (std::size_t offset = 1; offset < targetLine.caretXs.size(); ++offset) {
        if (targetX < (targetLine.caretXs[offset - 1] + targetLine.caretXs[offset]) * 0.5f) {
            return targetLine.startIndex + offset - 1;
        }
    }
    return targetLine.endIndex;
}

std::size_t TextLayout::visualLineStart(std::size_t caretIndex) const
{
    return lines.empty() ? 0 : lineForCaret(*this, caretIndex).startIndex;
}

std::size_t TextLayout::visualLineEnd(std::size_t caretIndex) const
{
    return lines.empty() ? 0 : lineForCaret(*this, caretIndex).endIndex;
}

TextLayoutRect TextLayout::caretRect(std::size_t caretIndex, float width) const
{
    if (lines.empty()) {
        return {};
    }
    const TextLayoutLine& line = lineForCaret(*this, caretIndex);
    return {
        .x = caretX(line, caretIndex),
        .y = line.y,
        .width = width,
        .height = lineHeight,
    };
}

std::vector<TextLayoutRect> TextLayout::selectionRects(std::size_t startIndex, std::size_t endIndex) const
{
    if (startIndex > endIndex) {
        std::swap(startIndex, endIndex);
    }
    std::vector<TextLayoutRect> result;
    for (const TextLayoutLine& line : lines) {
        const std::size_t start = std::max(startIndex, line.startIndex);
        const std::size_t end = std::min(endIndex, line.endIndex);
        if (end <= start) {
            continue;
        }
        const float left = caretX(line, start);
        const float right = caretX(line, end);
        result.push_back({
            .x = left,
            .y = line.y,
            .width = right - left,
            .height = lineHeight,
        });
    }
    return result;
}

} // namespace womp
