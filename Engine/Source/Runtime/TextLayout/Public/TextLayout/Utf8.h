#pragma once

#include <Core/Text/StringView.h>

// 검증된 UTF-8의 code point 해독 도우미다(TextLayout·backend 내부용 — byte 위치가 필요해 Core
// 반복자 대신).
namespace rocks::text {

// text[offset]에서 code point 하나를 읽어 codePoint에 쓰고 byte 수(1–4)를 낸다. 범위 밖이거나
// 잘못된 시작 byte면 0을 낸다(입력은 ValidateUtf8을 통과했다고 본다).
inline usize DecodeUtf8(StringView text, usize offset, char32_t& codePoint) noexcept {
    const usize size = text.ByteCount();
    if (offset >= size) {
        return 0;
    }

    const auto* bytes = reinterpret_cast<const unsigned char*>(text.Data());
    const unsigned char lead = bytes[offset];
    usize length = 0;
    char32_t value = 0;
    if (lead < 0x80) {
        codePoint = lead;
        return 1;
    }

    if ((lead & 0xE0) == 0xC0) {
        length = 2;
        value = lead & 0x1F;
    } else if ((lead & 0xF0) == 0xE0) {
        length = 3;
        value = lead & 0x0F;
    } else if ((lead & 0xF8) == 0xF0) {
        length = 4;
        value = lead & 0x07;
    } else {
        return 0;
    }

    if (offset + length > size) {
        return 0;
    }

    for (usize index = 1; index < length; ++index) {
        value = (value << 6) | (bytes[offset + index] & 0x3F);
    }

    codePoint = value;
    return length;
}

} // namespace rocks::text
