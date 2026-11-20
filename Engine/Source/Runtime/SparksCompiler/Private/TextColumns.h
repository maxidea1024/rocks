#pragma once

#include <Base/Types.h>

// 원문 열 계산의 공용 규칙이다(Docs/Design/SparksLanguage.ko.md 8.1절 "위치"). Source(줄·열)와
// Diagnostic(밑줄 폭)이 같은 규칙을 써야 밑줄이 열과 맞는다.

namespace rocks::sparks::detail {

// UTF-8 continuation byte(10xxxxxx)다.
inline constexpr uint8 Utf8ContinuationMask = 0xC0u;
inline constexpr uint8 Utf8ContinuationTag = 0x80u;

constexpr bool IsUtf8ContinuationByte(char value) noexcept {
    return (static_cast<uint8>(value) & Utf8ContinuationMask) == Utf8ContinuationTag;
}

// [begin, end)의 열 수다. continuation이 아닌 byte를 센다(유효한 UTF-8에서는 code point 수와 같다).
inline uint32 CountColumns(const char* begin, const char* end) noexcept {
    uint32 count = 0;
    for (const char* cursor = begin; cursor != end; ++cursor) {
        if (!IsUtf8ContinuationByte(*cursor)) {
            ++count;
        }
    }

    return count;
}

} // namespace rocks::sparks::detail
