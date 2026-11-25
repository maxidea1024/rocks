#include "RichTextMarkup.h"

namespace rocks::ui::detail {
namespace {

bool Space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

Result<void> Append(RichTextMarkup& output, StringView text, const RichTextStyle* style,
                    const PaintGlyph* image = nullptr) noexcept {
    if (text.IsEmpty()) {
        return Result<void>::Success();
    }
    auto appended = output.plain.TryAppend(text);
    if (!appended) {
        return appended;
    }
    if (!image && !output.runs.IsEmpty()) {
        auto& last = output.runs[output.runs.Size() - 1];
        if (!last.embedded && last.style == style) {
            last.end = output.plain.View().ByteCount();
            return Result<void>::Success();
        }
    }
    MarkupRun run{.end = output.plain.View().ByteCount(), .style = style};
    if (image) {
        run.image = *image;
        run.embedded = true;
    }
    return output.runs.TryPushBack(run);
}

} // namespace

Result<void> ParseRichTextMarkup(StringView input, Span<const RichTextStyle> styles,
                                 RichTextDecorator* decorator, float size,
                                 RichTextMarkup& output) noexcept {
    Array<const RichTextStyle*> stack(output.runs.GetAllocator());
    const char* bytes = input.Data();
    usize i = 0;
    while (i < input.ByteCount()) {
        const auto* active = stack.IsEmpty() ? nullptr : stack[stack.Size() - 1];
        const usize begin = i;
        if (bytes[i] == '&') {
            const auto rest = input.SliceBytes(i, input.ByteCount() - i);
            struct Entity {
                StringView name;
                StringView value;
            };
            constexpr Entity entities[] = {{"&lt;", "<"}, {"&gt;", ">"}, {"&amp;", "&"}};
            StringView decoded = "&";
            ++i;
            for (const auto& entity : entities) {
                if (rest.ByteCount() >= entity.name.ByteCount() &&
                    rest.SliceBytes(0, entity.name.ByteCount()) == entity.name) {
                    decoded = entity.value;
                    i = begin + entity.name.ByteCount();
                    break;
                }
            }
            auto added = Append(output, decoded, active);
            if (!added) {
                return added;
            }
            continue;
        }
        if (bytes[i] != '<') {
            do {
                ++i;
            } while (i < input.ByteCount() && bytes[i] != '<' && bytes[i] != '&');
            auto added = Append(output, input.SliceBytes(begin, i - begin), active);
            if (!added) {
                return added;
            }
            continue;
        }

        // 속성 문자열 안의 '>'는 태그 끝이 아니다.
        char quote = 0;
        for (++i; i < input.ByteCount(); ++i) {
            if (quote) {
                if (bytes[i] == quote) {
                    quote = 0;
                }
            } else if (bytes[i] == '"' || bytes[i] == '\'') {
                quote = bytes[i];
            } else if (bytes[i] == '>') {
                break;
            }
        }
        if (i == input.ByteCount() || i == begin + 1) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
        const usize close = i++;
        if (bytes[begin + 1] == '/') {
            const auto name = input.SliceBytes(begin + 2, close - begin - 2);
            if (stack.IsEmpty() ||
                (!name.IsEmpty() && stack[stack.Size() - 1]->name.View() != name)) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
            (void)stack.TryPopBack();
            continue;
        }
        const bool embedded = bytes[close - 1] == '/';
        const usize end = embedded ? close - 1 : close;
        usize sep = begin + 1;
        while (sep < end && !Space(bytes[sep])) {
            ++sep;
        }
        const auto name = input.SliceBytes(begin + 1, sep - begin - 1);
        if (name.IsEmpty()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
        while (sep < end && Space(bytes[sep])) {
            ++sep;
        }
        if (embedded) {
            if (!decorator) {
                return Result<void>::Failure(ErrorCode::NotReady);
            }
            auto icon = decorator->Resolve(name, input.SliceBytes(sep, end - sep), size);
            if (!icon) {
                return Result<void>::Failure(icon.Error());
            }
            const auto& glyph = icon.Value();
            if (glyph.resource.IsNull() || !glyph.position.IsFinite() || !glyph.size.IsFinite() ||
                glyph.size.x <= 0 || glyph.size.y <= 0 || !glyph.tint.IsFinite() ||
                !glyph.uv.IsValid()) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
            auto added = Append(output, "\xEF\xBF\xBC", active, &glyph);
            if (!added) {
                return added;
            }
            continue;
        }
        if (sep != end) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
        const RichTextStyle* style = nullptr;
        for (const auto& candidate : styles) {
            if (candidate.name.View() == name) {
                style = &candidate;
                break;
            }
        }
        if (!style) {
            return Result<void>::Failure(ErrorCode::NotFound);
        }
        auto pushed = stack.TryPushBack(style);
        if (!pushed) {
            return pushed;
        }
    }
    return stack.IsEmpty() ? Result<void>::Success()
                           : Result<void>::Failure(ErrorCode::InvalidArgument);
}

} // namespace rocks::ui::detail
