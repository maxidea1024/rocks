#pragma once

// ARM64 전용 구현이다. 다른 CPU에서 단독 include하면 아무 선언도 만들지 않는다.
#if defined(_M_ARM64) || defined(__aarch64__)

#include <Platform/Math/ClangPlatformMath.h>

#include <arm_neon.h>

namespace rocks::platform {

// AArch64 기본 명령만 사용한다. 정확 함수는 Base와 같은 결과를 내도록 범위를 먼저 검사한다.
// AArch64 변환 명령은 범위 밖을 포화하고 NaN을 0으로 만들므로 그 결과를 그대로 노출하지 않는다.
// 근사 함수는 frsqrte + frsqrts 2회 보정이며 CPU 구현마다 근사 bit가 다를 수 있다.
template <typename Base>
struct NeonPlatformMath : Base {
    using Base::CeilToInt32;
    using Base::CeilToInt64;
    using Base::FloorToInt32;
    using Base::FloorToInt64;
    using Base::InvSqrtEstimate;
    using Base::RoundToInt32;
    using Base::RoundToInt64;
    using Base::Sqrt;
    using Base::TruncToInt32;
    using Base::TruncToInt64;

    static float Sqrt(float value) noexcept {
        return vget_lane_f32(vsqrt_f32(vdup_n_f32(value)), 0);
    }

    static double Sqrt(double value) noexcept {
        return vget_lane_f64(vsqrt_f64(vdup_n_f64(value)), 0);
    }

    // 양의 정규 유한값만 근사 경로로 보낸다. 그 밖의 입력은 정확한 InvSqrt와 같다.
    static float InvSqrtEstimate(float value) noexcept {
        if (!(value >= 1.17549435e-38f && value < 3.40282347e+38f)) {
            return Base::InvSqrt(value);
        }
        float estimate = vrsqrtes_f32(value);
        estimate = estimate * vrsqrtss_f32(value * estimate, estimate);
        estimate = estimate * vrsqrtss_f32(value * estimate, estimate);
        return estimate;
    }

    static double InvSqrtEstimate(double value) noexcept {
        return Base::InvSqrt(value);
    }

    // AArch64 기본 명령 fcvtn/fcvtl로 변환한다. FPCR 기본 반올림(RNE)과 default-NaN 꺼짐을
    // 전제한다.
    static uint16 FloatToHalf(float value) noexcept {
        const float16x4_t converted = vcvt_f16_f32(vdupq_n_f32(value));
        return vget_lane_u16(vreinterpret_u16_f16(converted), 0);
    }

    static float HalfToFloat(uint16 value) noexcept {
        return vgetq_lane_f32(vcvt_f32_f16(vreinterpret_f16_u16(vdup_n_u16(value))), 0);
    }

    // float의 -2^31 이상 2^31 미만이면 네 가지 반올림 결과가 모두 int32에 들어간다.
    static int32 TruncToInt32(float value) noexcept {
        CheckFloatInt32(value);
        return vcvts_s32_f32(value);
    }

    static int32 FloorToInt32(float value) noexcept {
        CheckFloatInt32(value);
        return vcvtms_s32_f32(value);
    }

    static int32 CeilToInt32(float value) noexcept {
        CheckFloatInt32(value);
        return vcvtps_s32_f32(value);
    }

    // fcvtas는 0.5 경계를 0에서 멀어지는 방향으로 반올림하여 Generic 계약과 같다.
    static int32 RoundToInt32(float value) noexcept {
        CheckFloatInt32(value);
        return vcvtas_s32_f32(value);
    }

    // double은 64-bit 변환 뒤 int32 범위를 검사한다. NaN은 변환 전에 거부한다.
    static int32 TruncToInt32(double value) noexcept {
        return NarrowInt32(value, vcvtd_s64_f64(value));
    }

    static int32 FloorToInt32(double value) noexcept {
        return NarrowInt32(value, vcvtmd_s64_f64(value));
    }

    static int32 CeilToInt32(double value) noexcept {
        return NarrowInt32(value, vcvtpd_s64_f64(value));
    }

    static int32 RoundToInt32(double value) noexcept {
        return NarrowInt32(value, vcvtad_s64_f64(value));
    }

    static int64 TruncToInt64(double value) noexcept {
        CheckDoubleInt64(value);
        return vcvtd_s64_f64(value);
    }

    static int64 FloorToInt64(double value) noexcept {
        CheckDoubleInt64(value);
        return vcvtmd_s64_f64(value);
    }

    static int64 CeilToInt64(double value) noexcept {
        CheckDoubleInt64(value);
        return vcvtpd_s64_f64(value);
    }

    static int64 RoundToInt64(double value) noexcept {
        CheckDoubleInt64(value);
        return vcvtad_s64_f64(value);
    }

private:
    static void CheckFloatInt32(float value) noexcept {
        if (!(value >= -2147483648.0f && value < 2147483648.0f)) {
            Fatal("PlatformMath int32 conversion is NaN or out of range");
        }
    }

    // [-2^63, 2^63) 밖의 double은 반올림 결과도 int64 밖이다.
    static void CheckDoubleInt64(double value) noexcept {
        if (!(value >= -9223372036854775808.0 && value < 9223372036854775808.0)) {
            Fatal("PlatformMath int64 conversion is NaN or out of range");
        }
    }

    static int32 NarrowInt32(double value, int64 converted) noexcept {
        if (Base::IsNaN(value) || converted < -2147483648ll || converted > 2147483647ll) {
            Fatal("PlatformMath int32 conversion is NaN or out of range");
        }
        return static_cast<int32>(converted);
    }
};

} // namespace rocks::platform

#endif
