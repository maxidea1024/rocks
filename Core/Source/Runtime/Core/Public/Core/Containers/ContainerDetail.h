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

// UE 5.8의 기본 slack 정책을 Rocks의 checked 크기로 구현한다(allocator quantization 없음).
// 최초는 max(4, required), 이후는 required + floor(required * 3/8) + 16이다.
// maximum에서 포화한다. required 자체가 한도를 넘으면 caller의 할당 검사가 걸러야 한다.
constexpr usize GrowthCapacityWithin(usize capacity, usize required, usize maximum) noexcept {
    if (required > maximum) {
        return required;
    }

    if (capacity == 0) {
        return maximum < 4 || required > 4 ? required : 4;
    }
    const usize slack = (required / 8) * 3 + ((required % 8) * 3) / 8;
    if (slack > maximum - required || maximum - required - slack < 16) {
        return maximum;
    }

    return required + slack + 16;
}

// UE의 축소 hysteresis: 16KiB 이상 낭비 또는 사용률 2/3 미만이고, 여유가 64개 초과일 때만
// 줄인다(빈 컨테이너는 반환). 곱셈 overflow 없이 동일한 경계를 판정한다.
constexpr usize ShrinkCapacity(usize capacity, usize required, usize elementSize) noexcept {
    if (required >= capacity) {
        return capacity;
    }

    const usize slack = capacity - required;
    const bool tooManyBytes = elementSize != 0 && slack > (16384 - 1) / elementSize;
    const bool tooManyElements = required / 2 < slack;
    return (tooManyBytes || tooManyElements) && (slack > 64 || required == 0) ? required : capacity;
}

} // namespace rocks::detail
