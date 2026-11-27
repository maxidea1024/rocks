#include <UiWidget/RichTextImageDecorator.h>

namespace rocks::ui {

namespace {

// 이미지 태그 속성의 최소 길이다(id="x"처럼 키 2 + = + 따옴표 둘).
constexpr usize MinImageAttributeBytes = 5;

bool IsSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

} // namespace

RichTextImageDecorator::RichTextImageDecorator(Allocator& allocator) noexcept
    : entries_(allocator) {
}

Result<void> RichTextImageDecorator::SetImage(StringView id, const PaintGlyph& image) noexcept {
    if (id.IsEmpty() || image.resource.IsNull() || !image.size.IsFinite() || image.size.x <= 0 ||
        image.size.y <= 0 || !image.position.IsFinite() || !image.uv.IsValid() ||
        !image.tint.IsFinite()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (auto& entry : entries_) {
        if (entry.id.View() == id) {
            entry.image = image;
            return Result<void>::Success();
        }
    }

    Entry entry(entries_.GetAllocator());
    auto assigned = entry.id.TryAssign(id);
    if (!assigned) {
        return assigned;
    }

    entry.image = image;
    return entries_.TryPushBack(Move(entry));
}

Result<PaintGlyph> RichTextImageDecorator::Resolve(StringView tag, StringView attributes,
                                                   float) noexcept {
    if (tag != "img") {
        return Result<PaintGlyph>::Failure(ErrorCode::NotFound);
    }

    const auto count = attributes.ByteCount();
    const auto* data = attributes.Data();
    if (count < MinImageAttributeBytes || attributes.SliceBytes(0, 2) != "id") {
        return Result<PaintGlyph>::Failure(ErrorCode::InvalidArgument);
    }

    usize i = 2;
    while (i < count && IsSpace(data[i])) {
        ++i;
    }

    if (i == count || data[i++] != '=') {
        return Result<PaintGlyph>::Failure(ErrorCode::InvalidArgument);
    }
    while (i < count && IsSpace(data[i])) {
        ++i;
    }

    if (i == count || (data[i] != '"' && data[i] != '\'')) {
        return Result<PaintGlyph>::Failure(ErrorCode::InvalidArgument);
    }

    const char quote = data[i++];
    const usize begin = i;
    while (i < count && data[i] != quote) {
        ++i;
    }

    if (i == count || i == begin) {
        return Result<PaintGlyph>::Failure(ErrorCode::InvalidArgument);
    }

    const auto id = attributes.SliceBytes(begin, i++ - begin);
    while (i < count && IsSpace(data[i])) {
        ++i;
    }

    if (i != count) {
        return Result<PaintGlyph>::Failure(ErrorCode::InvalidArgument);
    }

    for (const auto& entry : entries_) {
        if (entry.id.View() == id) {
            return Result<PaintGlyph>::Success(entry.image);
        }
    }

    return Result<PaintGlyph>::Failure(ErrorCode::NotFound);
}

} // namespace rocks::ui
