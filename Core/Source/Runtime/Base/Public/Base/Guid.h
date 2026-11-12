#pragma once

#include <Base/Types.h>

namespace rocks {

// 두 64-bit 값으로 식별자를 보관하며 고유값 발급·중복 검사·유효성 판정은 하지 않는다.
// 영(0) 식별자의 의미와 직렬화 byte 순서는 이 타입을 사용하는 schema가 정한다.
struct Guid {
    uint64 high = 0;
    uint64 low = 0;

    friend constexpr bool operator==(const Guid&, const Guid&) noexcept = default;
};

} // namespace rocks
