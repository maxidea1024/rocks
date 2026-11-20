#include "SchemaInput.h"

#include <SparksCompiler/SyntaxTree.h>
#include <SparksCompiler/Token.h>

#include "Names.h"

namespace rocks::sparks::detail {

namespace {

// 숫자·리터럴 토큰에 들어가는 글자다(JSON5의 16진·Infinity·지수 포함).
constexpr bool IsJsonTokenCharacter(char value) noexcept {
    return IsAsciiLetterOrDigit(value) || value == '.' || value == '+' || value == '-';
}

// text[index]부터 표기 하나를 읽는다. 성공하면 index는 표기 다음 자리다.
bool ReadNotation(StringView text, usize& index, uint32 depth) noexcept {
    if (depth > MaxSyntaxNesting) {
        return false;
    }

    // 이름 {'.' 이름}
    while (true) {
        const usize begin = index;
        while (index < text.ByteCount() && IsAsciiLetterOrDigit(text.Data()[index])) {
            ++index;
        }

        const StringView name(text.Data() + begin, index - begin);
        if (!IsAsciiName(name) || IsKeywordText(name)) {
            return false;
        }

        if (index < text.ByteCount() && text.Data()[index] == '.') {
            ++index;
            continue;
        }

        break;
    }

    if (index < text.ByteCount() && text.Data()[index] == '<') {
        ++index;
        while (true) {
            if (!ReadNotation(text, index, depth + 1)) {
                return false;
            }

            if (index < text.ByteCount() && text.Data()[index] == '>') {
                ++index;
                break;
            }

            // 쉼표 뒤 공백 한 칸.
            if (index + 1 >= text.ByteCount() || text.Data()[index] != ',' ||
                text.Data()[index + 1] != ' ') {
                return false;
            }

            index += 2;
        }
    }

    if (index < text.ByteCount() && text.Data()[index] == '?') {
        ++index;
    }

    return true;
}

} // namespace

SourceSpan JsonValueSpan(StringView text, JsonPosition position) noexcept {
    const usize begin = static_cast<usize>(position.byteOffset);
    if (begin >= text.ByteCount()) {
        return SourceSpan{static_cast<uint32>(text.ByteCount()),
                          static_cast<uint32>(text.ByteCount())};
    }

    const char first = text.Data()[begin];
    usize end = begin + 1;
    if (first == '"' || first == '\'') {
        while (end < text.ByteCount() && text.Data()[end] != first && text.Data()[end] != '\n') {
            end += text.Data()[end] == '\\' && end + 1 < text.ByteCount() ? 2 : 1;
        }

        if (end < text.ByteCount() && text.Data()[end] == first) {
            ++end;
        }
    } else if (IsJsonTokenCharacter(first)) {
        while (end < text.ByteCount() && IsJsonTokenCharacter(text.Data()[end])) {
            ++end;
        }
    }

    return SourceSpan{static_cast<uint32>(begin), static_cast<uint32>(end)};
}

bool IsValidTypeNotation(StringView text) noexcept {
    usize index = 0;
    return ReadNotation(text, index, 0) && index == text.ByteCount();
}

} // namespace rocks::sparks::detail
