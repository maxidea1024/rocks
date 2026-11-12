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
//   if (!TryAlignUp(offset, 16, aligned)) { return Overflow; }             // 16의 배수로 올림
//
// 규칙:
// - Try* 함수는 성공할 때만 output을 갱신하고 true를 반환한다. 실패하면 output을 바꾸지 않는다.
// - 반환값을 버리면 컴파일 경고(오류)다([[nodiscard]]). 실패를 무시하면 output이 이전 값으로
// 남는다.
// - 결과 타입은 output의 타입이다. 인자 타입이 output과 다르면 각 인자를 먼저 output 타입으로 값이
//   바뀌지 않게 변환한다(TryNarrow). 그래서 int -1 × sizeof(T)를 usize로 계산하면 큰 수가 아니라
//   false다. usize·uint64처럼 플랫폼마다 다른 typedef를 섞어도 같은 코드가 모든 플랫폼에서
//   컴파일된다.
namespace rocks {

namespace detail {

template <typename T>
inline constexpr bool IsCheckedInteger = __is_integral(T) && !__is_same(T, bool);

template <typename T>
inline constexpr bool IsCheckedUnsigned = IsCheckedInteger<T> && !__is_signed(T);

// 인자 둘 중 하나라도 결과 타입과 다를 때만 혼합 타입 함수를 고른다(같으면 동일 타입 함수가
// 받는다).
template <typename T, typename L, typename R>
inline constexpr bool IsMixed = IsCheckedInteger<T> && IsCheckedInteger<L> && IsCheckedInteger<R> &&
                                (!__is_same(L, T) || !__is_same(R, T));

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

// 정수 value를 다른 정수 타입 To로 값이 바뀌지 않을 때만 바꾼다. 예: int64 70000 → int16은 false,
// int32 -1 → uint32는 false, uint64 5 → int8은 5.
template <typename To, typename From>
    requires detail::IsCheckedInteger<To> && detail::IsCheckedInteger<From>
[[nodiscard]] constexpr bool TryNarrow(From value, To& output) noexcept {
    const To converted = static_cast<To>(value);
    // 값이 같고 부호가 같아야 한다(-1 → 0xFFFFFFFF는 다시 바꾸면 같지만 부호가 바뀐다).
    if (static_cast<From>(converted) != value ||
        detail::IsNegative(converted) != detail::IsNegative(value)) {
        return false;
    }

    output = converted;
    return true;
}

// left + right. 결과가 T의 범위를 넘으면 false다. 부호 있는 타입은 음수 쪽 overflow도 검사한다.
template <typename T>
    requires detail::IsCheckedInteger<T>
[[nodiscard]] constexpr bool TryAdd(T left, T right, T& output) noexcept {
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
[[nodiscard]] constexpr bool TrySubtract(T left, T right, T& output) noexcept {
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
[[nodiscard]] constexpr bool TryMultiply(T left, T right, T& output) noexcept {
    T result{};
    if (__builtin_mul_overflow(left, right, &result)) {
        return false;
    }

    output = result;
    return true;
}

// 인자 타입이 output과 다른 덧셈·뺄셈·곱셈이다. 예: TryAdd(size, 1, out), TryMultiply(count,
// sizeof(Item), bytes). 각 인자가 output 타입(T)으로 표현되지 않으면(예: usize 결과에 음수 인자)
// 계산 결과가 범위 안이어도 false다.
template <typename T, typename L, typename R>
    requires detail::IsMixed<T, L, R>
[[nodiscard]] constexpr bool TryAdd(L left, R right, T& output) noexcept {
    T convertedLeft{};
    T convertedRight{};
    return TryNarrow(left, convertedLeft) && TryNarrow(right, convertedRight) &&
           TryAdd(convertedLeft, convertedRight, output);
}

template <typename T, typename L, typename R>
    requires detail::IsMixed<T, L, R>
[[nodiscard]] constexpr bool TrySubtract(L left, R right, T& output) noexcept {
    T convertedLeft{};
    T convertedRight{};
    return TryNarrow(left, convertedLeft) && TryNarrow(right, convertedRight) &&
           TrySubtract(convertedLeft, convertedRight, output);
}

template <typename T, typename L, typename R>
    requires detail::IsMixed<T, L, R>
[[nodiscard]] constexpr bool TryMultiply(L left, R right, T& output) noexcept {
    T convertedLeft{};
    T convertedRight{};
    return TryNarrow(left, convertedLeft) && TryNarrow(right, convertedRight) &&
           TryMultiply(convertedLeft, convertedRight, output);
}

// value를 alignment(0이 아닌 2의 거듭제곱)의 배수로 올린다. 예: (13, 8) → 16, (16, 8) → 16.
// 결과 타입은 부호 없는 output 타입(T)이다. alignment가 잘못됐거나, 인자가 T로 표현되지
// 않거나(음수), 올린 값이 T를 넘으면 false다.
template <typename T, typename V, typename A>
    requires detail::IsCheckedUnsigned<T> && detail::IsCheckedInteger<V> &&
             detail::IsCheckedInteger<A>
[[nodiscard]] constexpr bool TryAlignUp(V value, A alignment, T& output) noexcept {
    T convertedValue{};
    T convertedAlignment{};
    if (!TryNarrow(value, convertedValue) || !TryNarrow(alignment, convertedAlignment) ||
        convertedAlignment == 0 || (convertedAlignment & (convertedAlignment - 1)) != 0) {
        return false;
    }

    const T mask = static_cast<T>(convertedAlignment - 1);
    if (convertedValue > static_cast<T>(static_cast<T>(~T{}) - mask)) {
        return false;
    }

    output = static_cast<T>((convertedValue + mask) & static_cast<T>(~mask));
    return true;
}

// value가 alignment(0이 아닌 2의 거듭제곱)의 배수인지다. 잘못된 alignment와 음수 value는 false다.
template <typename V, typename A>
    requires detail::IsCheckedInteger<V> && detail::IsCheckedInteger<A>
[[nodiscard]] constexpr bool IsAligned(V value, A alignment) noexcept {
    uint64 convertedValue = 0;
    uint64 convertedAlignment = 0;
    return TryNarrow(value, convertedValue) && TryNarrow(alignment, convertedAlignment) &&
           convertedAlignment != 0 && (convertedAlignment & (convertedAlignment - 1)) == 0 &&
           (convertedValue & (convertedAlignment - 1)) == 0;
}

// value << shift. shift가 타입 폭 이상이거나, 잃는 bit가 있거나, 부호 있는 타입의 부호가 바뀌면
// false다. 부호 있는 음수 value도 거부한다(C++20부터 정의된 동작이지만 크기·offset 계산에서는 음수
// shift가 거의 항상 오류이므로 정책으로 막는다).
template <typename T>
    requires detail::IsCheckedInteger<T>
[[nodiscard]] constexpr bool TryShiftLeft(T value, uint32 shift, T& output) noexcept {
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
