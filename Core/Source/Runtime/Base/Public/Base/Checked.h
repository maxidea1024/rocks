#pragma once

#include <Base/Types.h>

// 정수 overflow·정렬·변환을 검사하는 산술이다. 크기 계산, 외부 데이터에서 온 길이·offset, 직렬화한
// 정수를 다룰 때 wrap된 값으로 작은 buffer를 할당하거나 잘린 값을 쓰는 실수를 막는다.
//
// 사용 예:
//   usize bytes = 0;
//   if (!TryMultiply(count, sizeof(Item), bytes)) { return Overflow; }   // count × 크기
//   int32 narrow = 0;
//   if (!TryNarrow(fileLength, narrow)) { return Overflow; }               // int64 → int32
//   usize aligned = 0;
//   TryAlignUp(offset, 16, aligned);                                       // 16의 배수로 올림
//
// 규칙: Try* 함수는 성공할 때만 output을 갱신하고 true를 반환한다. 실패하면 output을 바꾸지 않는다.
namespace rocks {

namespace detail {

template <typename T>
inline constexpr bool IsCheckedInteger = __is_integral(T) && !__is_same(T, bool);

// 부호 없는 타입에서 "항상 거짓" 비교 경고가 나지 않게 부호 있는 타입만 비교한다.
template <typename T>
constexpr bool IsNegative(T value) noexcept {
    if constexpr (__is_signed(T)) {
        return value < T{};
    } else {
        return false;
    }
}

} // namespace detail

// 0이 아닌 2의 거듭제곱인지다(정렬 인자 검사). 0은 거듭제곱으로 보지 않는다.
constexpr bool IsPowerOfTwo(usize value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

// 크기(usize) 덧셈·곱셈이다. 인자 타입이 섞여도(예: TryAdd(size, 1, out)) usize로 바꿔 계산한다.
constexpr bool TryAdd(usize left, usize right, usize& output) noexcept {
    if (right > MaxSize - left) {
        return false;
    }

    output = left + right;
    return true;
}

constexpr bool TryMultiply(usize left, usize right, usize& output) noexcept {
    if (left != 0 && right > MaxSize / left) {
        return false;
    }

    output = left * right;
    return true;
}

// left + right. 결과가 T의 범위를 넘으면 false다. 부호 있는 타입은 음수 쪽 overflow도 검사한다.
template <typename T>
    requires detail::IsCheckedInteger<T>
constexpr bool TryAdd(T left, T right, T& output) noexcept {
    T result{};
    if (__builtin_add_overflow(left, right, &result)) {
        return false;
    }

    output = result;
    return true;
}

// left - right. 부호 없는 타입에서 결과가 음수가 되면 false다.
template <typename T>
    requires detail::IsCheckedInteger<T>
constexpr bool TrySubtract(T left, T right, T& output) noexcept {
    T result{};
    if (__builtin_sub_overflow(left, right, &result)) {
        return false;
    }

    output = result;
    return true;
}

// left × right. 곱셈을 먼저 수행해 wrap된 크기로 작은 buffer를 할당하는 일을 막는다.
template <typename T>
    requires detail::IsCheckedInteger<T>
constexpr bool TryMultiply(T left, T right, T& output) noexcept {
    T result{};
    if (__builtin_mul_overflow(left, right, &result)) {
        return false;
    }

    output = result;
    return true;
}

// value를 alignment(0이 아닌 2의 거듭제곱)의 배수로 올린다. 예: (13, 8) → 16, (16, 8) → 16.
// alignment가 잘못됐거나 올린 값이 usize를 넘으면 false다.
constexpr bool TryAlignUp(usize value, usize alignment, usize& output) noexcept {
    if (!IsPowerOfTwo(alignment) || value > MaxSize - (alignment - 1)) {
        return false;
    }

    output = (value + (alignment - 1)) & ~(alignment - 1);
    return true;
}

// value가 alignment(0이 아닌 2의 거듭제곱)의 배수인지다. 잘못된 alignment는 false다.
constexpr bool IsAligned(usize value, usize alignment) noexcept {
    return IsPowerOfTwo(alignment) && (value & (alignment - 1)) == 0;
}

// 정수 value를 다른 정수 타입 To로 값이 바뀌지 않을 때만 바꾼다. 예: int64 70000 → int16은 false,
// int32 -1 → uint32는 false, uint64 5 → int8은 5.
template <typename To, typename From>
    requires detail::IsCheckedInteger<To> && detail::IsCheckedInteger<From>
constexpr bool TryNarrow(From value, To& output) noexcept {
    const To converted = static_cast<To>(value);
    // 값이 같고 부호가 같아야 한다(-1 → 0xFFFFFFFF는 다시 바꾸면 같지만 부호가 바뀐다).
    if (static_cast<From>(converted) != value ||
        detail::IsNegative(converted) != detail::IsNegative(value)) {
        return false;
    }

    output = converted;
    return true;
}

// value << shift. shift가 타입 폭 이상이거나, 잃는 bit가 있거나, 부호 있는 타입의 부호가 바뀌면
// false다. 부호 있는 음수 value는 정의되지 않은 동작을 피하려고 거부한다.
template <typename T>
    requires detail::IsCheckedInteger<T>
constexpr bool TryShiftLeft(T value, uint32 shift, T& output) noexcept {
    constexpr uint32 Bits = sizeof(T) * 8;
    if (shift >= Bits || detail::IsNegative(value)) {
        return false;
    }

    const T result = static_cast<T>(value << shift);
    if (static_cast<T>(result >> shift) != value || detail::IsNegative(result)) {
        return false;
    }

    output = result;
    return true;
}

} // namespace rocks
