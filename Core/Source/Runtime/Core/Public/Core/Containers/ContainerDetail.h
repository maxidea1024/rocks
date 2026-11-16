#pragma once

#include <Base/Types.h>

// Core 컨테이너가 같이 쓰는 내부 도우미다(공개 API가 아니다 — rocks::detail).
namespace rocks::detail {

// [a, a + aSize)와 [b, b + bSize)가 겹치는지 본다. 삽입 인자가 컨테이너 저장소나 다른 인자의 직접
// 별칭인지 검사할 때 쓴다. 서로 다른 객체의 포인터를 직접 순서 비교하거나 끝 주소를 더하지 않도록
// 정수 주소의 차이로만 판정한다(끝 주소 계산의 overflow도 없다). 크기 0인 범위는 겹치지 않는다.
inline bool AddressRangesOverlap(uptr a, usize aSize, uptr b, usize bSize) noexcept {
    return a <= b ? b - a < aSize : a - b < bSize;
}

// 연속 저장소 컨테이너(Array·SparseArray)가 버퍼를 늘릴 때 고르는 새 용량이다. 지금 용량
// capacity에서 required개를 담아야 할 때, 빈 저장소는 4(required가 더 크면 required), 그 밖은
// capacity의 1.5배 + 1(required가 더 크면 required)이며 maximum에서 포화한다. maximum은 그
// 컨테이너가 담을 수 있는 원소 수 상한이고, required <= maximum은 caller가 보장한다(넘는 required는
// 그대로 돌려주므로 caller의 할당 크기 검사가 Overflow로 거른다).
constexpr usize GrowthCapacityWithin(usize capacity, usize required, usize maximum) noexcept {
    if (capacity == 0) {
        return maximum < 4 || required > 4 ? required : 4;
    }

    const usize growth = capacity / 2 + 1;
    const usize candidate = growth > maximum - capacity ? maximum : capacity + growth;
    return candidate < required ? required : candidate;
}

} // namespace rocks::detail
