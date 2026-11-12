#pragma once

// x64 전용 구현이다. 다른 CPU에서 단독 include하면 아무 선언도 만들지 않는다.
#if defined(_M_X64) || defined(__x86_64__)

#include <Platform/Math/ClangPlatformMath.h>

#include <emmintrin.h>

namespace rocks::platform {

// x64 baseline인 SSE2 명령만 사용한다. SSE4.1/AVX는 CPU 기능 확인 없이 실행하지 않는다.
// 정확 함수는 Base와 bit 단위로 같은 결과를 낸다. 근사 함수만 InvSqrtEstimateRelativeError
// 안에서 다르며, rsqrtss 근사값은 Intel/AMD CPU 세대마다 다를 수 있다.
// UE의 SSE FloorToInt처럼 2x±0.5 변환과 현재 반올림 모드에 의존하는 방식은 범위·경계 오차가
// 있어 사용하지 않는다. 절사 명령과 정확한 보정만 사용한다.
template <typename Base>
struct SsePlatformMath : Base {
    using Base::CeilToInt32;
    using Base::CeilToInt64;
    using Base::FloorToInt32;
    using Base::FloorToInt64;
    using Base::InvSqrt;
    using Base::InvSqrtEstimate;
    using Base::RoundToInt32;
    using Base::RoundToInt64;
    using Base::Sqrt;
    using Base::TruncToInt32;
    using Base::TruncToInt64;

    static float Sqrt(float value) noexcept {
        return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(value)));
    }

    static double Sqrt(double value) noexcept {
        const __m128d input = _mm_set_sd(value);
        return _mm_cvtsd_f64(_mm_sqrt_sd(input, input));
    }

    static float InvSqrt(float value) noexcept {
        return _mm_cvtss_f32(_mm_div_ss(_mm_set_ss(1.0f), _mm_sqrt_ss(_mm_set_ss(value))));
    }

    static double InvSqrt(double value) noexcept {
        const __m128d input = _mm_set_sd(value);
        return _mm_cvtsd_f64(_mm_div_sd(_mm_set_sd(1.0), _mm_sqrt_sd(input, input)));
    }

    // 12-bit rsqrtss 근사에 Newton-Raphson 1회를 적용한다. rsqrtss는 subnormal을 0처럼 다루고
    // 0/Inf/NaN에서 NR 식이 NaN을 만들 수 있으므로 양의 정규 유한값만 근사 경로로 보낸다.
    static float InvSqrtEstimate(float value) noexcept {
        if (!(value >= 1.17549435e-38f && value < 3.40282347e+38f)) {
            return InvSqrt(value);
        }
        const __m128 input = _mm_set_ss(value);
        const __m128 half = _mm_set_ss(0.5f);
        const __m128 estimate = _mm_rsqrt_ss(input);
        const __m128 halfInput = _mm_mul_ss(input, half);
        // x1 = x0 + x0 * (0.5 - (y/2) * x0^2)
        const __m128 correction =
            _mm_sub_ss(half, _mm_mul_ss(halfInput, _mm_mul_ss(estimate, estimate)));
        return _mm_cvtss_f32(_mm_add_ss(estimate, _mm_mul_ss(estimate, correction)));
    }

    static double InvSqrtEstimate(double value) noexcept {
        return InvSqrt(value);
    }

    // cvttss2si는 NaN·범위 초과에서 INT32_MIN을 반환한다. 그 값만 Base의 검사 경로로 보낸다.
    // |x| < 2^23이면 절사값을 float로 되돌려도 정확하고, 그 이상의 float는 이미 정수다.
    static int32 TruncToInt32(float value) noexcept {
        const int32 truncated = _mm_cvttss_si32(_mm_set_ss(value));
        if (truncated == Int32Min) {
            return Base::TruncToInt32(value);
        }
        return truncated;
    }

    static int32 FloorToInt32(float value) noexcept {
        const int32 truncated = _mm_cvttss_si32(_mm_set_ss(value));
        if (truncated == Int32Min) {
            return Base::FloorToInt32(value);
        }
        return static_cast<float>(truncated) > value ? truncated - 1 : truncated;
    }

    // 결과가 INT32_MAX를 넘는 float 입력은 절사 단계에서 이미 INT32_MIN이 된다.
    static int32 CeilToInt32(float value) noexcept {
        const int32 truncated = _mm_cvttss_si32(_mm_set_ss(value));
        if (truncated == Int32Min) {
            return Base::CeilToInt32(value);
        }
        return static_cast<float>(truncated) < value ? truncated + 1 : truncated;
    }

    // value - trunc(value)는 같은 부호의 값에서 하위 bit만 지운 차이이므로 정확하다.
    static int32 RoundToInt32(float value) noexcept {
        const int32 truncated = _mm_cvttss_si32(_mm_set_ss(value));
        if (truncated == Int32Min) {
            return Base::RoundToInt32(value);
        }
        const float fraction = value - static_cast<float>(truncated);
        if (fraction >= 0.5f) {
            return CheckedStep(truncated, 1);
        }
        if (fraction <= -0.5f) {
            return CheckedStep(truncated, -1);
        }
        return truncated;
    }

    // double은 int32 경계 근처에도 소수부가 있으므로 보정 단계의 overflow를 다시 검사한다.
    static int32 TruncToInt32(double value) noexcept {
        const int32 truncated = _mm_cvttsd_si32(_mm_set_sd(value));
        if (truncated == Int32Min) {
            return Base::TruncToInt32(value);
        }
        return truncated;
    }

    static int32 FloorToInt32(double value) noexcept {
        const int32 truncated = _mm_cvttsd_si32(_mm_set_sd(value));
        if (truncated == Int32Min) {
            return Base::FloorToInt32(value);
        }
        return static_cast<double>(truncated) > value ? CheckedStep(truncated, -1) : truncated;
    }

    static int32 CeilToInt32(double value) noexcept {
        const int32 truncated = _mm_cvttsd_si32(_mm_set_sd(value));
        if (truncated == Int32Min) {
            return Base::CeilToInt32(value);
        }
        return static_cast<double>(truncated) < value ? CheckedStep(truncated, 1) : truncated;
    }

    static int32 RoundToInt32(double value) noexcept {
        const int32 truncated = _mm_cvttsd_si32(_mm_set_sd(value));
        if (truncated == Int32Min) {
            return Base::RoundToInt32(value);
        }
        const double fraction = value - static_cast<double>(truncated);
        if (fraction >= 0.5) {
            return CheckedStep(truncated, 1);
        }
        if (fraction <= -0.5) {
            return CheckedStep(truncated, -1);
        }
        return truncated;
    }

    // cvttsd2si(64-bit)는 범위 밖에서 INT64_MIN을 반환한다. |x| < 2^52 밖의 double은 정수다.
    static int64 TruncToInt64(double value) noexcept {
        const int64 truncated = _mm_cvttsd_si64(_mm_set_sd(value));
        if (truncated == Int64Min) {
            return Base::TruncToInt64(value);
        }
        return truncated;
    }

    static int64 FloorToInt64(double value) noexcept {
        const int64 truncated = _mm_cvttsd_si64(_mm_set_sd(value));
        if (truncated == Int64Min) {
            return Base::FloorToInt64(value);
        }
        return static_cast<double>(truncated) > value ? truncated - 1 : truncated;
    }

    static int64 CeilToInt64(double value) noexcept {
        const int64 truncated = _mm_cvttsd_si64(_mm_set_sd(value));
        if (truncated == Int64Min) {
            return Base::CeilToInt64(value);
        }
        return static_cast<double>(truncated) < value ? truncated + 1 : truncated;
    }

    static int64 RoundToInt64(double value) noexcept {
        const int64 truncated = _mm_cvttsd_si64(_mm_set_sd(value));
        if (truncated == Int64Min) {
            return Base::RoundToInt64(value);
        }
        const double fraction = value - static_cast<double>(truncated);
        if (fraction >= 0.5) {
            return truncated + 1;
        }
        if (fraction <= -0.5) {
            return truncated - 1;
        }
        return truncated;
    }

private:
    static constexpr int32 Int32Min = -2147483647 - 1;
    static constexpr int32 Int32Max = 2147483647;
    static constexpr int64 Int64Min = -9223372036854775807ll - 1;

    // 보정 결과가 int32 범위를 넘으면 Base와 같은 Fatal 진단으로 끝낸다.
    // 예: double 2147483647.5의 Ceil/Round 결과 2^31은 표현할 수 없다.
    static int32 CheckedStep(int32 truncated, int32 step) noexcept {
        if ((step > 0 && truncated == Int32Max) || (step < 0 && truncated == Int32Min)) {
            Fatal("PlatformMath int32 conversion is NaN or out of range");
        }
        return truncated + step;
    }
};

} // namespace rocks::platform

#endif
