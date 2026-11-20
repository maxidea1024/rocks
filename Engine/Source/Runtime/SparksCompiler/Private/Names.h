#pragma once

#include <Base/Types.h>
#include <Core/Text/StringView.h>

// Sparks 이름 규칙의 글자 검사다(Docs/Design/SparksLanguage.ko.md 3.1절 명명, 3.4절 패키지·모듈
// 경로, 10.2절 binding schema). 이름은 ASCII 영문자로 시작하고 ASCII 영문자·숫자만 쓴다(`_` 없음).

namespace rocks::sparks::detail {

constexpr bool IsUpperAsciiLetter(char value) noexcept {
    return value >= 'A' && value <= 'Z';
}

constexpr bool IsLowerAsciiLetter(char value) noexcept {
    return value >= 'a' && value <= 'z';
}

constexpr bool IsAsciiLetterOrDigit(char value) noexcept {
    return IsUpperAsciiLetter(value) || IsLowerAsciiLetter(value) || (value >= '0' && value <= '9');
}

// 영문자로 시작하고 영문자·숫자만 쓰는 이름이다.
constexpr bool IsAsciiName(StringView text) noexcept {
    if (text.IsEmpty() ||
        !(IsUpperAsciiLetter(text.Data()[0]) || IsLowerAsciiLetter(text.Data()[0]))) {
        return false;
    }

    for (usize index = 1; index < text.ByteCount(); ++index) {
        if (!IsAsciiLetterOrDigit(text.Data()[index])) {
            return false;
        }
    }

    return true;
}

// 영문 대문자로 시작하는 이름이다(타입·멤버·패키지·모듈 구간, PascalCase).
constexpr bool IsPascalName(StringView text) noexcept {
    return IsAsciiName(text) && IsUpperAsciiLetter(text.Data()[0]);
}

// 영문 소문자로 시작하는 이름이다(지역 이름·인자, lowerCamel).
constexpr bool IsCamelName(StringView text) noexcept {
    return IsAsciiName(text) && IsLowerAsciiLetter(text.Data()[0]);
}

// '.'로 이은 PascalCase 구간들이다(빈 구간 없음). 구간 수를 segments에 쓴다.
constexpr bool IsDottedPascalName(StringView text, usize& segments) noexcept {
    segments = 0;
    usize begin = 0;
    for (usize index = 0; index <= text.ByteCount(); ++index) {
        if (index != text.ByteCount() && text.Data()[index] != '.') {
            continue;
        }

        if (!IsPascalName(StringView(text.Data() + begin, index - begin))) {
            return false;
        }

        ++segments;
        begin = index + 1;
    }

    return true;
}

} // namespace rocks::sparks::detail
