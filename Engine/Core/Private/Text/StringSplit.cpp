#include <Core/Text/StringBuilder.h>
#include <Core/Text/StringSplit.h>

namespace rocks {

Result<usize> TrySplitIntoArray(StringView text, StringView delimiter, Array<StringView>& out,
                                SplitOptions options) noexcept {
    if (delimiter.IsEmpty()) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }
    const usize original = out.Size();
    usize added = 0;
    usize start = 0;
    const usize bytes = text.ByteCount();
    for (;;) {
        const StringView rest(text.Data() + start, bytes - start);
        const usize found = rest.Find(delimiter, options.comparison);
        const usize length = found == StringView::NotFound ? rest.ByteCount() : found;
        StringView piece(rest.Data(), length);
        if (options.trimAsciiWhitespace) {
            piece = piece.TrimAscii();
        }
        if (!piece.IsEmpty() || !options.cullEmpty) {
            auto pushed = out.TryPushBack(piece);
            if (!pushed) {
                // 줄이는 Resize는 할당하지 않으므로 실패하지 않는다.
                (void)out.TryResize(original);
                return Result<usize>::Failure(pushed.Error());
            }
            ++added;
        }
        if (found == StringView::NotFound) {
            break;
        }
        start += found + delimiter.ByteCount();
    }
    return Result<usize>::Success(added);
}

Result<String> TryJoin(Allocator& allocator, Span<const StringView> parts,
                       StringView separator) noexcept {
    StringBuilder<256> builder(allocator);
    auto joined = builder.TryAppendJoin(parts, separator);
    if (!joined) {
        return Result<String>::Failure(joined.Error());
    }
    return builder.TryToString(allocator);
}

} // namespace rocks
