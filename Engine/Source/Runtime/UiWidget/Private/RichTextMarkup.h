#pragma once

#include <UiWidget/RichTextDecorator.h>
#include <UiWidget/RichTextStyle.h>

namespace rocks::ui::detail {

struct MarkupRun {
    usize end = 0;
    const RichTextStyle* style = nullptr;
    PaintGlyph image{};
    bool embedded = false;
};

struct RichTextMarkup {
    explicit RichTextMarkup(Allocator& allocator) noexcept : plain(allocator), runs(allocator) {
    }

    String plain;
    Array<MarkupRun> runs;
};

Result<void> ParseRichTextMarkup(StringView markup, Span<const RichTextStyle> styles,
                                 RichTextDecorator* decorator, float size,
                                 RichTextMarkup& output) noexcept;

} // namespace rocks::ui::detail
