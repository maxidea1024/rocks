#pragma once

#include <Base/Types.h>
#include <Platform/Fatal.h>

#include <float.h>
#include <math.h>
#include <Platform/Memory.h>

namespace rocks::platform {

// 모든 CPU에서 같은 의미를 갖는 기준 scalar 수학 구현이다.
// CPU별 구현(SsePlatformMath, NeonPlatformMath)은 이 타입을 상속해 같은 계약의 함수만 대체한다.
// 사용자는 이 타입 대신 <Platform/PlatformMath.h>의 PlatformMath 또는 Core의 math:: 함수를 쓴다.
//
// 공통 계약:
// - float/double 입력을 같은 정밀도로 계산한다. 결과는 IEEE-754 binary32/64 기준이다.
// - NaN·무한대는 함수별 규약대로 전파한다. 잘못된 입력을 조용히 0으로 바꾸지 않는다.
// - 정수 변환은 결과가 대상 범위 밖이거나 입력이 NaN이면 Fatal이다. 실패를 값으로 다루려면
//   Try* 변형을 사용한다. CPU 변환 명령의 "integer indefinite" 값을 결과로 노출하지 않는다.
// - 근사 함수(*Estimate)는 정확한 함수와 이름으로 구분한다. 근사 오차는 CPU마다 다를 수 있다.
// - fast-math나 finite-math-only 설정을 전제하지 않는다. 분류 함수는 bit 표현을 직접 검사한다.
struct GenericPlatformMath {
    // ---- IEEE 특수 값(float·double) ----
    template <class T>
    static constexpr T QuietNaN() noexcept {
        static_assert(__is_same(T, float) || __is_same(T, double), "QuietNaN<float|double>");
        if constexpr (__is_same(T, float)) {
            return __builtin_nanf("");
        } else {
            return __builtin_nan("");
        }
    }

    template <class T>
    static constexpr T Infinity() noexcept {
        static_assert(__is_same(T, float) || __is_same(T, double), "Infinity<float|double>");
        if constexpr (__is_same(T, float)) {
            return __builtin_huge_valf();
        } else {
            return __builtin_huge_val();
        }
    }

    // ---- 분류: compiler 최적화 설정과 무관하게 bit 표현으로 판정한다. ----
    static constexpr bool IsNaN(float value) noexcept {
        return (platform::BitCast<uint32>(value) & 0x7fffffffu) > 0x7f800000u;
    }

    static constexpr bool IsNaN(double value) noexcept {
        return (platform::BitCast<uint64>(value) & 0x7fffffffffffffffull) > 0x7ff0000000000000ull;
    }

    // NaN·±무한대가 아니면 true다. subnormal과 ±0은 유한값이다.
    static constexpr bool IsFinite(float value) noexcept {
        return (platform::BitCast<uint32>(value) & 0x7f800000u) != 0x7f800000u;
    }

    static constexpr bool IsFinite(double value) noexcept {
        return (platform::BitCast<uint64>(value) & 0x7ff0000000000000ull) != 0x7ff0000000000000ull;
    }

    static constexpr bool IsInfinite(float value) noexcept {
        return (platform::BitCast<uint32>(value) & 0x7fffffffu) == 0x7f800000u;
    }

    static constexpr bool IsInfinite(double value) noexcept {
        return (platform::BitCast<uint64>(value) & 0x7fffffffffffffffull) == 0x7ff0000000000000ull;
    }

    // -0.0과 음수 NaN도 부호 bit가 1이면 true다.
    static constexpr bool SignBit(float value) noexcept {
        return (platform::BitCast<uint32>(value) >> 31) != 0;
    }

    static constexpr bool SignBit(double value) noexcept {
        return (platform::BitCast<uint64>(value) >> 63) != 0;
    }

    // 부호 bit만 지운다. NaN payload와 -0.0 → +0.0을 보존·정규화한다.
    static constexpr float Abs(float value) noexcept {
        return platform::BitCast<float>(platform::BitCast<uint32>(value) & 0x7fffffffu);
    }

    static constexpr double Abs(double value) noexcept {
        return platform::BitCast<double>(platform::BitCast<uint64>(value) & 0x7fffffffffffffffull);
    }

    // magnitude의 크기와 sign의 부호 bit를 결합한다.
    static constexpr float CopySign(float magnitude, float sign) noexcept {
        return platform::BitCast<float>((platform::BitCast<uint32>(magnitude) & 0x7fffffffu) |
                                        (platform::BitCast<uint32>(sign) & 0x80000000u));
    }

    static constexpr double CopySign(double magnitude, double sign) noexcept {
        return platform::BitCast<double>(
            (platform::BitCast<uint64>(magnitude) & 0x7fffffffffffffffull) |
            (platform::BitCast<uint64>(sign) & 0x8000000000000000ull));
    }

    // (a < b) ? a : b 로 정의한다. 둘 중 하나가 NaN이면 b를, -0.0과 +0.0은 b를 반환한다.
    // x64 minss/maxss의 피연산자 규약과 같게 고정하여 CPU 구현 간 결과를 일치시킨다.
    static constexpr float Min(float a, float b) noexcept {
        return a < b ? a : b;
    }

    static constexpr double Min(double a, double b) noexcept {
        return a < b ? a : b;
    }

    // (a > b) ? a : b 로 정의한다. NaN과 부호 있는 0의 규약은 Min과 같다.
    static constexpr float Max(float a, float b) noexcept {
        return a > b ? a : b;
    }

    static constexpr double Max(double a, double b) noexcept {
        return a > b ? a : b;
    }

    // ---- 제곱근: IEEE 정확 반올림이다. 음수는 NaN, -0.0은 -0.0, +Inf는 +Inf다. ----
    static float Sqrt(float value) noexcept {
        return ::sqrtf(value);
    }

    static double Sqrt(double value) noexcept {
        return ::sqrt(value);
    }

    // 1/sqrt(x)를 두 번의 정확 반올림으로 계산한다. +0은 +Inf, -0은 -Inf(sqrt(-0) = -0), +Inf는 +0,
    // 음수는 NaN이다.
    static float InvSqrt(float value) noexcept {
        return 1.0f / Sqrt(value);
    }

    static double InvSqrt(double value) noexcept {
        return 1.0 / Sqrt(value);
    }

    // 빠른 근사 경로다. 기준 구현은 정확한 InvSqrt와 같다. CPU 구현은 양의 정규 유한값에서
    // 상대 오차 InvSqrtEstimateRelativeError 이하를 보장하고, 그 밖의 입력은 InvSqrt와 같다.
    static float InvSqrtEstimate(float value) noexcept {
        return InvSqrt(value);
    }

    static double InvSqrtEstimate(double value) noexcept {
        return InvSqrt(value);
    }

    static constexpr float InvSqrtEstimateRelativeError = 4.0e-7f;

    // ---- 반올림: 결과는 같은 부동소수점 타입이다. NaN·Inf·±0은 그대로 전파된다. ----
    static float Floor(float value) noexcept {
        return ::floorf(value);
    }

    static double Floor(double value) noexcept {
        return ::floor(value);
    }

    static float Ceil(float value) noexcept {
        return ::ceilf(value);
    }

    static double Ceil(double value) noexcept {
        return ::ceil(value);
    }

    static float Trunc(float value) noexcept {
        return ::truncf(value);
    }

    static double Trunc(double value) noexcept {
        return ::trunc(value);
    }

    // 0.5 경계는 0에서 멀어지는 방향으로 반올림한다(C round와 같음). 현재 반올림 모드와 무관하다.
    // UE의 RoundToFloat는 +무한대 방향이므로 음수 x.5에서 결과가 다르다.
    static float Round(float value) noexcept {
        return ::roundf(value);
    }

    static double Round(double value) noexcept {
        return ::round(value);
    }

    // C fmod와 같다. 결과 부호는 dividend를 따르며 divisor가 0이면 NaN이다.
    static float Fmod(float dividend, float divisor) noexcept {
        return ::fmodf(dividend, divisor);
    }

    static double Fmod(double dividend, double divisor) noexcept {
        return ::fmod(dividend, divisor);
    }

    // ---- 초월 함수: 플랫폼 C 수학 라이브러리 결과다. CPU·OS 간 bit 동일성은 보장하지 않는다. ----
    static float Sin(float radians) noexcept {
        return ::sinf(radians);
    }

    static double Sin(double radians) noexcept {
        return ::sin(radians);
    }

    static float Cos(float radians) noexcept {
        return ::cosf(radians);
    }

    static double Cos(double radians) noexcept {
        return ::cos(radians);
    }

    static float Tan(float radians) noexcept {
        return ::tanf(radians);
    }

    static double Tan(double radians) noexcept {
        return ::tan(radians);
    }

    // 정의역 [-1, 1] 밖은 NaN이다. 오차로 범위를 살짝 벗어난 입력은 호출자가 먼저 clamp한다.
    static float Asin(float value) noexcept {
        return ::asinf(value);
    }

    static double Asin(double value) noexcept {
        return ::asin(value);
    }

    static float Acos(float value) noexcept {
        return ::acosf(value);
    }

    static double Acos(double value) noexcept {
        return ::acos(value);
    }

    static float Atan(float value) noexcept {
        return ::atanf(value);
    }

    static double Atan(double value) noexcept {
        return ::atan(value);
    }

    // 결과 범위는 [-pi, pi]다. Atan2(±0, -0)처럼 부호 있는 0의 결과는 C atan2를 따른다.
    static float Atan2(float y, float x) noexcept {
        return ::atan2f(y, x);
    }

    static double Atan2(double y, double x) noexcept {
        return ::atan2(y, x);
    }

    static float Exp(float value) noexcept {
        return ::expf(value);
    }

    static double Exp(double value) noexcept {
        return ::exp(value);
    }

    // 0은 -Inf, 음수는 NaN이다.
    static float Log(float value) noexcept {
        return ::logf(value);
    }

    static double Log(double value) noexcept {
        return ::log(value);
    }

    static float Log2(float value) noexcept {
        return ::log2f(value);
    }

    static double Log2(double value) noexcept {
        return ::log2(value);
    }

    static float Pow(float base, float exponent) noexcept {
        return ::powf(base, exponent);
    }

    static double Pow(double base, double exponent) noexcept {
        return ::pow(base, exponent);
    }

    // ---- 정수 변환: 결과가 int32/int64 범위 밖이거나 NaN이면 Fatal이다. ----
    // Try 변형은 같은 조건에서 false를 반환하고 output을 바꾸지 않는다.
    static bool TryTruncToInt32(double value, int32& output) noexcept {
        return TryToInt32(Trunc(value), output);
    }

    static bool TryFloorToInt32(double value, int32& output) noexcept {
        return TryToInt32(Floor(value), output);
    }

    static bool TryCeilToInt32(double value, int32& output) noexcept {
        return TryToInt32(Ceil(value), output);
    }

    static bool TryRoundToInt32(double value, int32& output) noexcept {
        return TryToInt32(Round(value), output);
    }

    static bool TryTruncToInt64(double value, int64& output) noexcept {
        return TryToInt64(Trunc(value), output);
    }

    static bool TryFloorToInt64(double value, int64& output) noexcept {
        return TryToInt64(Floor(value), output);
    }

    static bool TryCeilToInt64(double value, int64& output) noexcept {
        return TryToInt64(Ceil(value), output);
    }

    static bool TryRoundToInt64(double value, int64& output) noexcept {
        return TryToInt64(Round(value), output);
    }

    // float 입력은 double로 정확히 확장된 뒤 같은 규칙을 적용한다.
    static int32 TruncToInt32(float value) noexcept {
        return CheckedInt32(Trunc(static_cast<double>(value)));
    }

    static int32 TruncToInt32(double value) noexcept {
        return CheckedInt32(Trunc(value));
    }

    static int32 FloorToInt32(float value) noexcept {
        return CheckedInt32(Floor(static_cast<double>(value)));
    }

    static int32 FloorToInt32(double value) noexcept {
        return CheckedInt32(Floor(value));
    }

    static int32 CeilToInt32(float value) noexcept {
        return CheckedInt32(Ceil(static_cast<double>(value)));
    }

    static int32 CeilToInt32(double value) noexcept {
        return CheckedInt32(Ceil(value));
    }

    // 0.5 경계는 0에서 멀어지는 방향이다. UE RoundToInt(+무한대 방향)와 음수 x.5에서 다르다.
    static int32 RoundToInt32(float value) noexcept {
        return CheckedInt32(Round(static_cast<double>(value)));
    }

    static int32 RoundToInt32(double value) noexcept {
        return CheckedInt32(Round(value));
    }

    static int64 TruncToInt64(double value) noexcept {
        return CheckedInt64(Trunc(value));
    }

    static int64 FloorToInt64(double value) noexcept {
        return CheckedInt64(Floor(value));
    }

    static int64 CeilToInt64(double value) noexcept {
        return CheckedInt64(Ceil(value));
    }

    static int64 RoundToInt64(double value) noexcept {
        return CheckedInt64(Round(value));
    }

    // ---- IEEE binary16(반정밀도) 변환(UE StoreHalf/LoadHalf 대응) ----
    // float → half는 round-to-nearest-even이다. 65520 이상의 크기는 ±Inf, 2^-25 이하는 ±0이 된다.
    // NaN은 부호와 상위 payload를 보존하고 quiet bit를 켠다. 현재 부동소수점 반올림 모드와
    // 무관하다.
    static constexpr uint16 FloatToHalf(float value) noexcept {
        const uint32 bits = platform::BitCast<uint32>(value);
        const auto sign = static_cast<uint16>((bits >> 16) & 0x8000u);
        const uint32 magnitude = bits & 0x7fffffffu;
        if (magnitude >= 0x7f800000u) {
            if (magnitude == 0x7f800000u) {
                return static_cast<uint16>(sign | 0x7c00u);
            }
            return static_cast<uint16>(sign | 0x7e00u | ((magnitude >> 13) & 0x3ffu));
        }
        // 65520(0x477ff000) 이상은 half 최댓값 65504보다 반올림 중간점을 넘으므로 Inf다.
        if (magnitude >= 0x477ff000u) {
            return static_cast<uint16>(sign | 0x7c00u);
        }
        // half 최소 정규값 2^-14 미만은 2^-24 단위 subnormal로 반올림한다.
        if (magnitude < 0x38800000u) {
            const uint32 exponent = magnitude >> 23;
            if (exponent < 102u) {
                return sign; // 2^-25 미만은 0
            }
            const uint32 mantissa = (magnitude & 0x7fffffu) | 0x800000u;
            const uint32 shift = 126u - exponent;
            uint32 result = mantissa >> shift;
            const uint32 remainder = mantissa & ((1u << shift) - 1u);
            const uint32 halfway = 1u << (shift - 1u);
            if (remainder > halfway || (remainder == halfway && (result & 1u) != 0)) {
                ++result; // 0x400이 되면 최소 정규값의 올바른 인코딩이다.
            }
            return static_cast<uint16>(sign | result);
        }
        // 정규값: 지수 bias를 127에서 15로 바꾸고 하위 13 bit를 RTNE로 반올림한다.
        uint32 result = (magnitude - 0x38000000u) >> 13;
        const uint32 remainder = magnitude & 0x1fffu;
        if (remainder > 0x1000u || (remainder == 0x1000u && (result & 1u) != 0)) {
            ++result;
        }
        return static_cast<uint16>(sign | result);
    }

    // half → float는 항상 정확하다. signaling NaN은 quiet NaN이 된다.
    static constexpr float HalfToFloat(uint16 value) noexcept {
        const uint32 sign = static_cast<uint32>(value & 0x8000u) << 16;
        const uint32 exponent = (value >> 10) & 0x1fu;
        const uint32 mantissa = value & 0x3ffu;
        if (exponent == 0) {
            // subnormal과 0: mantissa * 2^-24는 float에서 정확하다.
            const float magnitude = static_cast<float>(mantissa) * 5.9604644775390625e-8f;
            return platform::BitCast<float>(platform::BitCast<uint32>(magnitude) | sign);
        }
        if (exponent == 31u) {
            const uint32 payload = mantissa == 0 ? 0u : (0x400000u | (mantissa << 13));
            return platform::BitCast<float>(sign | 0x7f800000u | payload);
        }
        return platform::BitCast<float>(sign | ((exponent + 112u) << 23) | (mantissa << 13));
    }

    // ---- 정수 bit 연산: 0 입력의 결과를 bit 폭으로 정의한다. ----
    static constexpr uint32 CountLeadingZeros32(uint32 value) noexcept {
        uint32 count = 0;
        for (uint32 mask = 0x80000000u; mask != 0 && (value & mask) == 0; mask >>= 1) {
            ++count;
        }
        return count;
    }

    static constexpr uint32 CountLeadingZeros64(uint64 value) noexcept {
        const uint32 high = CountLeadingZeros32(static_cast<uint32>(value >> 32));
        return high != 32 ? high : 32 + CountLeadingZeros32(static_cast<uint32>(value));
    }

    static constexpr uint32 CountTrailingZeros32(uint32 value) noexcept {
        if (value == 0) {
            return 32;
        }
        uint32 count = 0;
        while ((value & 1u) == 0) {
            value >>= 1;
            ++count;
        }
        return count;
    }

    static constexpr uint32 CountTrailingZeros64(uint64 value) noexcept {
        const uint32 low = CountTrailingZeros32(static_cast<uint32>(value));
        return low != 32 ? low : 32 + CountTrailingZeros32(static_cast<uint32>(value >> 32));
    }

    static constexpr uint32 PopCount32(uint32 value) noexcept {
        uint32 count = 0;
        while (value != 0) {
            value &= value - 1;
            ++count;
        }
        return count;
    }

    static constexpr uint32 PopCount64(uint64 value) noexcept {
        return PopCount32(static_cast<uint32>(value)) +
               PopCount32(static_cast<uint32>(value >> 32));
    }

protected:
    // 이미 정수로 반올림된 값이 int32에 정확히 들어가는지 검사한다. NaN은 비교가 false다.
    static bool TryToInt32(double rounded, int32& output) noexcept {
        if (!(rounded >= -2147483648.0 && rounded <= 2147483647.0)) {
            return false;
        }
        output = static_cast<int32>(rounded);
        return true;
    }

    // 2^63은 double로 정확하지만 int64 최댓값보다 크다. 상한은 미만 비교로 제외한다.
    static bool TryToInt64(double rounded, int64& output) noexcept {
        if (!(rounded >= -9223372036854775808.0 && rounded < 9223372036854775808.0)) {
            return false;
        }
        output = static_cast<int64>(rounded);
        return true;
    }

    static int32 CheckedInt32(double rounded) noexcept {
        int32 output = 0;
        if (!TryToInt32(rounded, output)) {
            Fatal("PlatformMath int32 conversion is NaN or out of range");
        }
        return output;
    }

    static int64 CheckedInt64(double rounded) noexcept {
        int64 output = 0;
        if (!TryToInt64(rounded, output)) {
            Fatal("PlatformMath int64 conversion is NaN or out of range");
        }
        return output;
    }
};

static_assert(FLT_RADIX == 2 && FLT_MANT_DIG == 24 && DBL_MANT_DIG == 53,
              "Rocks math requires IEEE-754 binary32/binary64");
static_assert(sizeof(float) == 4 && sizeof(double) == 8);

} // namespace rocks::platform
