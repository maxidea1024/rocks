#pragma once

#include <Base/Types.h>

namespace rocks {

// 정렬 인자의 공통 검사다. 0은 유효한 2의 거듭제곱으로 취급하지 않는다.
constexpr bool IsPowerOfTwo(usize value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

// 성공할 때만 output을 갱신한다. 실패 후 caller의 이전 크기·상태를 보존한다.
constexpr bool TryAdd(usize left, usize right, usize& output) noexcept {
    if (right > MaxSize - left) {
        return false;
    }

    output = left + right;
    return true;
}

// 곱셈을 먼저 수행하지 않아 wrap된 크기로 작은 buffer를 할당하는 일을 방지한다.
constexpr bool TryMultiply(usize left, usize right, usize& output) noexcept {
    if (left != 0 && right > MaxSize / left) {
        return false;
    }

    output = left * right;
    return true;
}

} // namespace rocks
