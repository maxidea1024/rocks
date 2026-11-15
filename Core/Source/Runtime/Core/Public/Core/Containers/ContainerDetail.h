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

} // namespace rocks::detail
