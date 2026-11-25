#pragma once

#include <Core/Math/MathUtility.h>
#include <Platform/PlatformMath.h>

// 감속 보간의 비율 변환이다(UE FMath::InterpEaseIn·InterpSinIn·InterpExpoIn·InterpCircularIn 계열 —
// UnrealMathUtility.h:1280-1395). 각 함수는 [0, 1] 비율 alpha를 바꾼 비율을 돌려준다. 값 보간은
// Lerp(a, b, 바꾼 비율)이다. UE와 같게 float로 계산한다.
//
// 사용 예:
//   const float eased = math::EaseInOutAlpha(alpha, 2.0f);
//   value = start + (end - start) * eased;
namespace rocks::math {

// alpha^exponent다(UE InterpEaseIn).
inline float EaseInAlpha(float alpha, float exponent) noexcept {
    return platform::PlatformMath::Pow(alpha, exponent);
}

// 1 − (1 − alpha)^exponent다(UE InterpEaseOut).
inline float EaseOutAlpha(float alpha, float exponent) noexcept {
    return 1.0f - platform::PlatformMath::Pow(1.0f - alpha, exponent);
}

// 앞 절반은 EaseIn, 뒤 절반은 EaseOut이다(UE InterpEaseInOut).
inline float EaseInOutAlpha(float alpha, float exponent) noexcept {
    return alpha < 0.5f ? EaseInAlpha(alpha * 2.0f, exponent) * 0.5f
                        : EaseOutAlpha(alpha * 2.0f - 1.0f, exponent) * 0.5f + 0.5f;
}

// 1 − cos(alpha·π/2)다(UE InterpSinIn).
inline float SinInAlpha(float alpha) noexcept {
    return -1.0f * Cos(alpha * HalfPi<float>) + 1.0f;
}

// sin(alpha·π/2)다(UE InterpSinOut).
inline float SinOutAlpha(float alpha) noexcept {
    return Sin(alpha * HalfPi<float>);
}

inline float SinInOutAlpha(float alpha) noexcept {
    return alpha < 0.5f ? SinInAlpha(alpha * 2.0f) * 0.5f
                        : SinOutAlpha(alpha * 2.0f - 1.0f) * 0.5f + 0.5f;
}

// alpha = 0이면 0, 아니면 2^(10(alpha − 1))다(UE InterpExpoIn).
inline float ExpoInAlpha(float alpha) noexcept {
    return alpha == 0.0f ? 0.0f : platform::PlatformMath::Pow(2.0f, 10.0f * (alpha - 1.0f));
}

// alpha = 1이면 1, 아니면 1 − 2^(−10·alpha)다(UE InterpExpoOut).
inline float ExpoOutAlpha(float alpha) noexcept {
    return alpha == 1.0f ? 1.0f : -platform::PlatformMath::Pow(2.0f, -10.0f * alpha) + 1.0f;
}

inline float ExpoInOutAlpha(float alpha) noexcept {
    return alpha < 0.5f ? ExpoInAlpha(alpha * 2.0f) * 0.5f
                        : ExpoOutAlpha(alpha * 2.0f - 1.0f) * 0.5f + 0.5f;
}

// 1 − √(1 − alpha²)다(UE InterpCircularIn).
inline float CircularInAlpha(float alpha) noexcept {
    return -1.0f * (Sqrt(1.0f - alpha * alpha) - 1.0f);
}

// √(1 − (alpha − 1)²)다(UE InterpCircularOut).
inline float CircularOutAlpha(float alpha) noexcept {
    const float shifted = alpha - 1.0f;
    return Sqrt(1.0f - shifted * shifted);
}

inline float CircularInOutAlpha(float alpha) noexcept {
    return alpha < 0.5f ? CircularInAlpha(alpha * 2.0f) * 0.5f
                        : CircularOutAlpha(alpha * 2.0f - 1.0f) * 0.5f + 0.5f;
}

} // namespace rocks::math
