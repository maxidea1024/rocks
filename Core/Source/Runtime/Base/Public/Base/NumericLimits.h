#pragma once

// 수치 형식의 표현 범위다(UE5.8 TNumericLimits 대응, Math/NumericLimits.h). 0xffffffffu·UINT32_MAX·
// SIZE_MAX·static_cast<usize>(-1)·~0u 같은 날 숫자 대신 형식 이름으로 한계를 적는다. 값은 모두
// 상수 식이며 OS·CRT 헤더(<stdint.h>·<limits.h>·<float.h>)와 STL(<limits>)에 의존하지 않는다.
// Base/Types.h가 이 헤더를 include하므로 Types.h를 쓰는 코드는 따로 include하지 않아도 된다.
//
// 사용 예:
//   if (slot->roots == NumericLimits<uint32>::Max()) { return Overflow; }   // 계수 포화
//   if (bytes > NumericLimits<uint64>::Max() - offset) { return Overflow; } // 합의 상한
//   float lowest = NumericLimits<float>::Lowest();                           // 가장 작은 유한값
//
// 규칙:
// - T는 비교·대입하는 값의 형식과 같게 쓴다. uint32 칸을 usize 최대값과 비교하면 항상 거짓이다.
// - 정수: Min()과 Lowest()는 같은 값(가장 작은 값)이고 부호 없는 형식은 0이다. bool·enum은 지원하지
//   않는다. char·wchar_t처럼 플랫폼마다 부호가 다른 형식은 그 플랫폼의 부호를 따른다.
// - 부동소수(float·double, IEEE 754 binary32·binary64): Max()는 가장 큰 유한값, Lowest()는
//   -Max()(가장 작은 유한값), Min()은 가장 작은 양의 정규 값이다(UE MIN_flt·STL numeric_limits와
//   같다). 0보다 작은 값을 기대하고 Min()을 쓰지 않는다. long double은 플랫폼마다 형식이 달라
//   지원하지 않는다.
// - const·volatile 한정 형식은 한정을 뗀 형식과 같다. 지원하지 않는 형식은 정의되지 않은 템플릿
//   사용으로 컴파일 오류다.
// - "없음" 표지(찾지 못한 색인·빈 슬롯)는 이 값을 직접 쓰지 않고 이름 있는 상수로 둔다. usize
//   색인은 Base/Types.h의 IndexNone, 다른 형식은 영역의 상수다(Docs/CodingStyle.md).
//
// 구현 메모: Types.h를 거쳐 거의 모든 번역 단위에 들어가므로 rocks 안에 보조 이름공간(detail 등)을
// 만들지 않는다. 다른 모듈이 using namespace로 끌어온 같은 이름의 이름공간과 모호해진다.
namespace rocks {

template <typename T>
struct NumericLimits;

template <typename T>
struct NumericLimits<const T> : NumericLimits<T> {};

template <typename T>
struct NumericLimits<volatile T> : NumericLimits<T> {};

template <typename T>
struct NumericLimits<const volatile T> : NumericLimits<T> {};

// 한정 없는 정수 형식(bool 제외)이다. 한정 형식은 위 const·volatile 특수화가 한정을 떼어 여기로
// 온다.
template <typename T>
    requires(__is_integral(T) && !__is_same(T, bool) && !__is_const(T) && !__is_volatile(T))
struct NumericLimits<T> {
    [[nodiscard]] static constexpr T Max() noexcept {
        if constexpr (__is_signed(T)) {
            // 부호 bit를 뺀 모든 bit가 1이다. 2^(N-2) - 1 + 2^(N-2)로 계산해 중간값이 넘치지
            // 않는다.
            constexpr T half = static_cast<T>(T{1} << (sizeof(T) * 8 - 2));
            return static_cast<T>(half - 1 + half);
        } else {
            return static_cast<T>(~T{});
        }
    }

    [[nodiscard]] static constexpr T Min() noexcept {
        if constexpr (__is_signed(T)) {
            return static_cast<T>(-Max() - 1);
        } else {
            return T{};
        }
    }

    [[nodiscard]] static constexpr T Lowest() noexcept {
        return Min();
    }
};

template <>
struct NumericLimits<float> {
    [[nodiscard]] static constexpr float Max() noexcept {
        return 0x1.fffffep+127f;
    }

    [[nodiscard]] static constexpr float Min() noexcept {
        return 0x1p-126f;
    }

    [[nodiscard]] static constexpr float Lowest() noexcept {
        return -Max();
    }
};

template <>
struct NumericLimits<double> {
    [[nodiscard]] static constexpr double Max() noexcept {
        return 0x1.fffffffffffffp+1023;
    }

    [[nodiscard]] static constexpr double Min() noexcept {
        return 0x1p-1022;
    }

    [[nodiscard]] static constexpr double Lowest() noexcept {
        return -Max();
    }
};

static_assert(sizeof(float) == 4 && sizeof(double) == 8);

} // namespace rocks
