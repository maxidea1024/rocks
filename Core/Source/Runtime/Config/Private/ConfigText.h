#pragma once

#include <Core/Text/StringView.h>

// Config 모듈 안에서 함께 쓰는 텍스트 비교다(모듈 밖에 공개하지 않는다).
namespace rocks::config::detail {

// byte 순서 비교(사전식)다. 이름 블록·대기 목록·dump 등 Config의 이름 정렬이 모두 이 순서다.
constexpr bool LessBytes(StringView left, StringView right) noexcept {
    const usize count = left.ByteCount() < right.ByteCount() ? left.ByteCount() : right.ByteCount();
    for (usize index = 0; index < count; ++index) {
        const auto a = static_cast<uint8>(left.Data()[index]);
        const auto b = static_cast<uint8>(right.Data()[index]);
        if (a != b) {
            return a < b;
        }
    }

    return left.ByteCount() < right.ByteCount();
}

} // namespace rocks::config::detail
