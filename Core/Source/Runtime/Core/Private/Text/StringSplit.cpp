#include <Core/Text/StringBuilder.h>
#include <Core/Text/StringSplit.h>

namespace rocks {

namespace {

constexpr char EscapeIntroducer = '\\';

// escape 글자 목록은 ASCII 0x21–0x7E만 받는다(SplitOptions::escapeCharacters 주석).
bool IsValidEscapeList(StringView escapeCharacters) noexcept {
    for (usize index = 0; index < escapeCharacters.ByteCount(); ++index) {
        const uint8 byte = static_cast<uint8>(escapeCharacters.Data()[index]);
        if (byte < 0x21 || byte > 0x7E) {
            return false;
        }
    }

    return true;
}

bool IsEscapable(StringView escapeCharacters, char character) noexcept {
    for (usize index = 0; index < escapeCharacters.ByteCount(); ++index) {
        if (escapeCharacters.Data()[index] == character) {
            return true;
        }
    }

    return false;
}

// text[index]에서 escape 쌍이 시작하는지 본다.
bool IsEscapePair(StringView text, usize index, StringView escapeCharacters) noexcept {
    return text.Data()[index] == EscapeIntroducer && index + 1 < text.ByteCount() &&
           IsEscapable(escapeCharacters, text.Data()[index + 1]);
}

// start부터 escape되지 않은 첫 delimiter의 위치(text 기준, 없으면 NotFound)다. 인자 검사는 호출자가
// 했다.
usize FindUnescapedFrom(StringView text, usize start, StringView delimiter,
                        StringView escapeCharacters, StringComparison comparison) noexcept {
    const usize bytes = text.ByteCount();
    if (escapeCharacters.IsEmpty()) {
        const usize found =
            StringView(text.Data() + start, bytes - start).Find(delimiter, comparison);
        return found == StringView::NotFound ? StringView::NotFound : start + found;
    }

    usize index = start;
    while (index < bytes) {
        // escape 쌍은 구분자 비교보다 먼저 본다(쌍의 두 byte는 구분자의 일부가 될 수 없다).
        if (IsEscapePair(text, index, escapeCharacters)) {
            index += 2;
            continue;
        }

        if (StringView(text.Data() + index, bytes - index).StartsWith(delimiter, comparison)) {
            return index;
        }

        ++index;
    }

    return StringView::NotFound;
}

} // namespace

Result<usize> TrySplitIntoArray(StringView text, StringView delimiter, Array<StringView>& out,
                                SplitOptions options) noexcept {
    if (delimiter.IsEmpty() || !IsValidEscapeList(options.escapeCharacters)) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }
    const usize original = out.Size();
    usize added = 0;
    usize start = 0;
    const usize bytes = text.ByteCount();
    for (;;) {
        const usize at =
            FindUnescapedFrom(text, start, delimiter, options.escapeCharacters, options.comparison);
        const usize found = at == StringView::NotFound ? StringView::NotFound : at - start;
        const StringView rest(text.Data() + start, bytes - start);
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

Result<usize> TryFindUnescaped(StringView text, StringView delimiter, StringView escapeCharacters,
                               StringComparison comparison) noexcept {
    if (delimiter.IsEmpty() || !IsValidEscapeList(escapeCharacters)) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<usize>::Success(
        FindUnescapedFrom(text, 0, delimiter, escapeCharacters, comparison));
}

Result<void> TryAppendUnescaped(StringBuilderBase& out, StringView piece,
                                StringView escapeCharacters) noexcept {
    if (!IsValidEscapeList(escapeCharacters)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // escape 쌍 사이의 구간을 통째로 붙이고 쌍은 뒤 글자만 붙인다.
    usize run = 0;
    usize index = 0;
    const usize bytes = piece.ByteCount();
    while (index < bytes) {
        if (!IsEscapePair(piece, index, escapeCharacters)) {
            ++index;
            continue;
        }

        if (auto appended = out.TryAppend(StringView(piece.Data() + run, index - run)); !appended) {
            return appended;
        }

        if (auto escaped = out.TryAppendAscii(piece.Data()[index + 1]); !escaped) {
            return escaped;
        }

        index += 2;
        run = index;
    }

    return out.TryAppend(StringView(piece.Data() + run, bytes - run));
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
