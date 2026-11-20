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

// UTF-8 sequence 하나의 code point와 byte 수다. text는 유효한 UTF-8이어야 한다(호출자가 이미 검사한
// 범위). available은 읽을 수 있는 byte 수이며 sequence가 그보다 길면 거기서 자른다.
inline uint32 DecodeCodePoint(const char* text, usize available, usize& width) noexcept {
    constexpr uint8 AsciiLimit = 0x80u;
    constexpr uint8 TwoByteMask = 0xE0u;
    constexpr uint8 TwoByteLead = 0xC0u;
    constexpr uint8 ThreeByteMask = 0xF0u;
    constexpr uint8 ThreeByteLead = 0xE0u;
    constexpr uint8 PayloadBits = 6;
    constexpr uint8 ContinuationPayload = 0x3Fu;
    const auto lead = static_cast<uint8>(text[0]);
    if (lead < AsciiLimit) {
        width = 1;
        return lead;
    }

    usize length = 4;
    uint32 value = lead & 0x07u;
    if ((lead & TwoByteMask) == TwoByteLead) {
        length = 2;
        value = lead & 0x1Fu;
    } else if ((lead & ThreeByteMask) == ThreeByteLead) {
        length = 3;
        value = lead & 0x0Fu;
    }

    if (length > available) {
        length = available;
    }

    for (usize index = 1; index < length; ++index) {
        value = (value << PayloadBits) | (static_cast<uint8>(text[index]) & ContinuationPayload);
    }

    width = length;
    return value;
}

} // namespace rocks::sparks::detail
