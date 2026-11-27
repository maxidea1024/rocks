#include <CurveAssets/ColorCurveEvaluate.h>
#include <CurveAssets/CurveAssetConstants.h>
#include <Core/Math/MathUtility.h>
#include <Curves/CurveMathDeterministic.h>
#include <DeterministicMath/DeterministicMath.h>
#include <Platform/Memory.h>
#include <Platform/PlatformMath.h>

// 색 커브 평가다(Docs/Design/CurveAssets 2.4.2·2.4.3절). UE
// UCurveLinearColor::GetAdjustedColorValue(Engine/Private/Curves/CurveLinearColor.cpp:81-142)와
// FLinearColor의 HSV 변환(Core/Private/Math/Color.cpp:383-443)을 float 그대로 옮긴다. 비교 도우미는
// UE 5.8.4 형태다(GenericPlatformMath.h:944-972, UnrealMathUtility.h:551-561·592-595 — 같은 값이면
// Max·Min이 오른쪽을 돌려주어 ±0 결과까지 같다). Core LinearColor의 HSV는 double·다른 경계 규칙이라
// 쓰지 않는다(설계 CAD10).
namespace rocks {

namespace {

// UE FMath::Max·Min(GenericPlatformMath — (B < A) ? A : B, (A < B) ? A : B).
float MaxOf(float left, float right) noexcept {
    return (right < left) ? left : right;
}

float MinOf(float left, float right) noexcept {
    return (left < right) ? left : right;
}

// UE FMath::Max3·Min3(Max(Max(A, B), C)).
float Max3(float first, float second, float third) noexcept {
    return MaxOf(MaxOf(first, second), third);
}

float Min3(float first, float second, float third) noexcept {
    return MinOf(MinOf(first, second), third);
}

// UE FMath::Clamp(Max(Min(X, high), low) — X가 NaN이면 high).
float ClampOf(float value, float low, float high) noexcept {
    return MaxOf(MinOf(value, high), low);
}

float AbsOf(float value) noexcept {
    return value < 0.0f ? -value : value;
}

// UE FMath::Lerp(a, b, t) = a + t × (b − a).
float LerpOf(float from, float to, float alpha) noexcept {
    return from + alpha * (to - from);
}

// UE 상수 60도(HSV 육각 구간)와 구간 수다.
constexpr float HueSectorDegrees = 60.0f;
constexpr uint32 HueSectorCount = 6;
// 구간마다 R·G·B가 쓰는 값 칸이다(UE RGBSwizzle).
constexpr uint32 HueSwizzle[HueSectorCount][3] = {{0, 3, 1}, {2, 0, 1}, {1, 0, 3},
                                                  {1, 2, 0}, {3, 1, 0}, {0, 1, 2}};

// UE FLinearColor::LinearRGBToHSV(r = 색상 도, g = 채도, b = 명도, a 유지).
LinearColor RgbToHsv(const LinearColor& color) noexcept {
    const float rgbMin = Min3(color.r, color.g, color.b);
    const float rgbMax = Max3(color.r, color.g, color.b);
    const float rgbRange = rgbMax - rgbMin;
    if (rgbMax <= 0.0f) {
        return LinearColor(0.0f, 0.0f, 0.0f, color.a);
    }

    float hue = 0.0f;
    if (rgbMax == rgbMin) {
        hue = 0.0f;
    } else if (rgbMax == color.r) {
        hue = platform::PlatformMath::Fmod((((color.g - color.b) / rgbRange) * HueSectorDegrees) +
                                               HueDegreesPerTurn,
                                           HueDegreesPerTurn);
    } else if (rgbMax == color.g) {
        hue = (((color.b - color.r) / rgbRange) * HueSectorDegrees) + 2.0f * HueSectorDegrees;
    } else if (rgbMax == color.b) {
        hue = (((color.r - color.g) / rgbRange) * HueSectorDegrees) + 4.0f * HueSectorDegrees;
    }

    const float saturation = rgbMax == 0.0f ? 0.0f : rgbRange / rgbMax;
    return LinearColor(hue, saturation, rgbMax, color.a);
}

// UE FLinearColor::HSVToLinearRGB(S는 [0, 1]로 자르고 V는 자르지 않는다).
LinearColor HsvToRgb(const LinearColor& hsv) noexcept {
    float hue = platform::PlatformMath::Fmod(hsv.r, HueDegreesPerTurn);
    if (hue < 0.0f) {
        hue += HueDegreesPerTurn;
    }

    const float saturation = ClampOf(hsv.g, 0.0f, 1.0f);
    const float value = hsv.b;
    const float sector = hue / HueSectorDegrees;
    // UE (uint32)(HDiv60)다. NaN 색상(비유한 입력)은 UE에서 정의되지 않은 변환이라 구간 0으로 둔다.
    const uint32 sectorFloor = sector >= 0.0f ? static_cast<uint32>(sector) : 0u;
    const float fraction = sector - static_cast<float>(sectorFloor);
    const float values[4] = {
        value,
        value * (1.0f - saturation),
        value * (1.0f - (fraction * saturation)),
        value * (1.0f - ((1.0f - fraction) * saturation)),
    };
    const uint32* swizzle = HueSwizzle[sectorFloor % HueSectorCount];
    return LinearColor(values[swizzle[0]], values[swizzle[1]], values[swizzle[2]], hsv.a);
}

// 런타임 수학(UE 비트 — 커브 Evaluate, 플랫폼 powf)이다.
struct PlatformColorMath {
    static float Evaluate(const TangentCurve& curve, float time) noexcept {
        return curve.Evaluate(time);
    }

    static float Pow(float base, float exponent) noexcept {
        return platform::PlatformMath::Pow(base, exponent);
    }

    static LinearColor Finish(const LinearColor& color) noexcept {
        return color;
    }
};

// 쿠킹 수학(결정적 — 설계 2.4.5절)이다.
struct DeterministicColorMath {
    static float Evaluate(const TangentCurve& curve, float time) noexcept {
        return curve.EvaluateDeterministic(time);
    }

    static float Pow(float base, float exponent) noexcept {
        return DeterministicPowf(base, exponent);
    }

    static LinearColor Finish(const LinearColor& color) noexcept {
        return LinearColor(
            CurveMathDeterministic::Canonical(color.r), CurveMathDeterministic::Canonical(color.g),
            CurveMathDeterministic::Canonical(color.b), CurveMathDeterministic::Canonical(color.a));
    }
};

template <class Math>
LinearColor Unadjusted(const TangentCurve& r, const TangentCurve& g, const TangentCurve& b,
                       const TangentCurve& a, float time) noexcept {
    const float alpha = a.KeyCount() == 0 ? 1.0f : Math::Evaluate(a, time);
    return LinearColor(Math::Evaluate(r, time), Math::Evaluate(g, time), Math::Evaluate(b, time),
                       alpha);
}

template <class Math>
LinearColor Adjusted(const TangentCurve& r, const TangentCurve& g, const TangentCurve& b,
                     const TangentCurve& a, const ColorCurveAdjustments& adjustments, float time,
                     bool clampAlways) noexcept {
    const LinearColor original = Unadjusted<Math>(r, g, b, a, time);
    // 원본 RGB가 LDR이면(퍼지) 명도를 [0, 1]로 자른다.
    const bool clampValue =
        clampAlways || Max3(original.r, original.g, original.b) < 1.0f + ColorAdjustTolerance;
    LinearColor hsv = RgbToHsv(original);
    float& hue = hsv.r;
    float& saturation = hsv.g;
    float& value = hsv.b;
    value *= adjustments.brightness;
    if (!(AbsOf(adjustments.brightnessCurve - 1.0f) <= ColorAdjustTolerance) &&
        adjustments.brightnessCurve != 0.0f) {
        value = Math::Pow(value, adjustments.brightnessCurve);
    }

    if (!(AbsOf(adjustments.vibrance) <= ColorAdjustTolerance)) {
        const float raised = Math::Pow(1.0f - saturation, VibranceSaturationExponent);
        const float clamped = ClampOf(adjustments.vibrance, 0.0f, 1.0f);
        const float half = clamped * VibranceScale;
        saturation += half * raised;
    }

    saturation *= adjustments.saturation;
    hue += adjustments.hue;
    if (clampValue) {
        value = ClampOf(value, 0.0f, 1.0f);
    }

    LinearColor result = HsvToRgb(hsv);
    result.a = LerpOf(adjustments.minAlpha, adjustments.maxAlpha, original.a);
    return result;
}

// 정수 float의 홀수 여부다(|y| ≥ 2^24인 float 정수는 모두 짝수다).
constexpr float FloatIntegerExact = 16777216.0f; // 2^24

// float ±Inf와 고정 quiet NaN의 비트다.
constexpr uint32 PositiveInfinityBits = 0x7F800000u;
constexpr uint32 NegativeInfinityBits = 0xFF800000u;
constexpr uint32 CanonicalNaNBits = 0x7FC00000u;

bool IsInteger(float value) noexcept {
    return deterministic::Trunc(static_cast<double>(value)) == static_cast<double>(value);
}

bool IsOddInteger(float value) noexcept {
    const float magnitude = AbsOf(value);
    return magnitude < FloatIntegerExact && IsInteger(value) &&
           (static_cast<uint32>(magnitude) & 1u) != 0;
}

} // namespace

LinearColor EvaluateUnadjustedColorCurve(const TangentCurve& r, const TangentCurve& g,
                                         const TangentCurve& b, const TangentCurve& a,
                                         float time) noexcept {
    return Unadjusted<PlatformColorMath>(r, g, b, a, time);
}

LinearColor EvaluateAdjustedColorCurve(const TangentCurve& r, const TangentCurve& g,
                                       const TangentCurve& b, const TangentCurve& a,
                                       const ColorCurveAdjustments& adjustments, float time,
                                       bool clampAlways) noexcept {
    return Adjusted<PlatformColorMath>(r, g, b, a, adjustments, time, clampAlways);
}

LinearColor EvaluateUnadjustedColorCurveDeterministic(const TangentCurve& r, const TangentCurve& g,
                                                      const TangentCurve& b, const TangentCurve& a,
                                                      float time) noexcept {
    return DeterministicColorMath::Finish(Unadjusted<DeterministicColorMath>(r, g, b, a, time));
}

LinearColor EvaluateAdjustedColorCurveDeterministic(const TangentCurve& r, const TangentCurve& g,
                                                    const TangentCurve& b, const TangentCurve& a,
                                                    const ColorCurveAdjustments& adjustments,
                                                    float time, bool clampAlways) noexcept {
    return DeterministicColorMath::Finish(
        Adjusted<DeterministicColorMath>(r, g, b, a, adjustments, time, clampAlways));
}

float DeterministicPowf(float base, float exponent) noexcept {
    // C powf 특수값(C11 F.10.4.4)을 먼저 본다.
    if (exponent == 0.0f || base == 1.0f) {
        return 1.0f;
    }

    if (math::IsNaN(base) || math::IsNaN(exponent)) {
        return CurveMathDeterministic::Canonical(base + exponent);
    }

    const bool infiniteExponent = !math::IsFinite(exponent);
    if (base == -1.0f && infiniteExponent) {
        return 1.0f;
    }

    if (base == 0.0f) {
        // ±0 밑: 홀수 정수 지수면 부호를 지킨다.
        const bool keepSign = IsOddInteger(exponent);
        const float zero = keepSign ? base : 0.0f;
        if (exponent < 0.0f) {
            return platform::BitCast<float>(keepSign && (platform::BitCast<uint32>(base) >> 31) != 0
                                                ? NegativeInfinityBits
                                                : PositiveInfinityBits);
        }

        return zero;
    }

    const double magnitude = static_cast<double>(AbsOf(base));
    const float raised =
        static_cast<float>(deterministic::Pow(magnitude, static_cast<double>(exponent)));
    if (base > 0.0f) {
        return CurveMathDeterministic::Canonical(raised);
    }

    // −Inf 밑(C11 F.10.4.4): 홀수 정수 지수면 부호를 지키고(y > 0 → −Inf, y < 0 → −0), 그 밖은
    // y > 0 → +Inf, y < 0 → +0이다(raised = |밑|^y가 이미 +Inf·+0이다).
    if (!math::IsFinite(base)) {
        return IsOddInteger(exponent) ? -raised : raised;
    }

    // 유한 음수 밑: 정수 지수(±Inf는 짝수로 본다)만 실수 결과다.
    if (!infiniteExponent && !IsInteger(exponent)) {
        return platform::BitCast<float>(CanonicalNaNBits);
    }

    return CurveMathDeterministic::Canonical(IsOddInteger(exponent) ? -raised : raised);
}

} // namespace rocks
