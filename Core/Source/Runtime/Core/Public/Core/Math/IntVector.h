#pragma once

#include <Base/Types.h>
#include <Platform/Fatal.h>

namespace rocks {

// 정수 격자 좌표다(UE FIntPoint/FIntVector 대응). 픽셀·셀·인덱스 좌표에 사용한다.
// 산술 overflow는 C++에서 정의되지 않은 동작이므로 감추지 않고 Fatal로 드러낸다.
// 나눗셈은 0 나눗셈과 INT32_MIN / -1을 Fatal로 처리하며 결과는 0 방향으로 절사한다.
namespace detail {

inline int32 CheckedAdd(int32 a, int32 b) noexcept {
    int32 result = 0;
    if (__builtin_add_overflow(a, b, &result)) {
        platform::Fatal("IntVector addition overflow");
    }
    return result;
}

inline int32 CheckedSubtract(int32 a, int32 b) noexcept {
    int32 result = 0;
    if (__builtin_sub_overflow(a, b, &result)) {
        platform::Fatal("IntVector subtraction overflow");
    }
    return result;
}

inline int32 CheckedMultiply(int32 a, int32 b) noexcept {
    int32 result = 0;
    if (__builtin_mul_overflow(a, b, &result)) {
        platform::Fatal("IntVector multiplication overflow");
    }
    return result;
}

inline int32 CheckedDivide(int32 a, int32 b) noexcept {
    if (b == 0 || (a == -2147483647 - 1 && b == -1)) {
        platform::Fatal("IntVector division by zero or overflow");
    }
    return a / b;
}

} // namespace detail

struct IntVec2 {
    int32 x = 0;
    int32 y = 0;

    constexpr IntVec2() noexcept = default;

    constexpr IntVec2(int32 xValue, int32 yValue) noexcept : x(xValue), y(yValue) {
    }

    friend constexpr bool operator==(const IntVec2&, const IntVec2&) noexcept = default;

    IntVec2 operator+(const IntVec2& other) const noexcept {
        return IntVec2(detail::CheckedAdd(x, other.x), detail::CheckedAdd(y, other.y));
    }

    IntVec2 operator-(const IntVec2& other) const noexcept {
        return IntVec2(detail::CheckedSubtract(x, other.x), detail::CheckedSubtract(y, other.y));
    }

    IntVec2 operator*(int32 scale) const noexcept {
        return IntVec2(detail::CheckedMultiply(x, scale), detail::CheckedMultiply(y, scale));
    }

    IntVec2 operator/(int32 divisor) const noexcept {
        return IntVec2(detail::CheckedDivide(x, divisor), detail::CheckedDivide(y, divisor));
    }
};

struct IntVec3 {
    int32 x = 0;
    int32 y = 0;
    int32 z = 0;

    constexpr IntVec3() noexcept = default;

    constexpr IntVec3(int32 xValue, int32 yValue, int32 zValue) noexcept
        : x(xValue), y(yValue), z(zValue) {
    }

    friend constexpr bool operator==(const IntVec3&, const IntVec3&) noexcept = default;

    IntVec3 operator+(const IntVec3& other) const noexcept {
        return IntVec3(detail::CheckedAdd(x, other.x), detail::CheckedAdd(y, other.y),
                       detail::CheckedAdd(z, other.z));
    }

    IntVec3 operator-(const IntVec3& other) const noexcept {
        return IntVec3(detail::CheckedSubtract(x, other.x), detail::CheckedSubtract(y, other.y),
                       detail::CheckedSubtract(z, other.z));
    }

    IntVec3 operator*(int32 scale) const noexcept {
        return IntVec3(detail::CheckedMultiply(x, scale), detail::CheckedMultiply(y, scale),
                       detail::CheckedMultiply(z, scale));
    }

    IntVec3 operator/(int32 divisor) const noexcept {
        return IntVec3(detail::CheckedDivide(x, divisor), detail::CheckedDivide(y, divisor),
                       detail::CheckedDivide(z, divisor));
    }
};

} // namespace rocks
