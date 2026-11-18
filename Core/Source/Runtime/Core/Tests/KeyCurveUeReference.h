#pragma once

#include <Base/NumericLimits.h>
#include <Base/Types.h>
#include <Core/Math/KeyCurveMath.h>
#include <Core/Math/MathUtility.h>
#include <Platform/PlatformMath.h>

// UE 5.8 키 커브 평가 식을 그대로 옮긴 시험 참조 구현이다(Docs/Design/CurveData.ko.md 16절). Core
// MathKeyCurveGoldenTests.cpp(수학 층)와 Curves/Tests의 커브 golden 시험(CurveGoldenTests.cpp —
// RichCurve·SimpleCurve 평가·편집 참조가 이 참조를 쓴다)이 함께 쓴다. 참조는 UE 원본을 줄 단위로
// 옮긴 것이고(형식·암묵 변환·연산 순서를 UE 소스 그대로 적었다) 설계 문서 15절의 수정을 Options로
// 하나씩 켜고 끈다. 참조의 FloorToInt는 Windows x64 UE의 SSE 식 cvt(F + F − 0.5f) >> 1이고,
// SinCos는 UE 다항식이다.
//
// 참조 함수 ↔ UE 5.8.4 원본(리뷰어 줄 단위 대조용):
//   ue58curve::FloorToInt32Sse        ↔ Runtime/Core/Public/Math/UnrealPlatformMathSSE.h 86–91
//   ue58curve::TruncToInt64           ↔ x64 cvttss2si(64비트) — UnrealMathUtility.h 766·770의
//                                       (int64) 변환
//   ue58curve::SinCos                 ↔ Runtime/Core/Public/Math/UnrealMathUtility.h 760–798
//   ue58curve::Lerp                   ↔ Runtime/Core/Public/Math/UnrealMathUtility.h 1119–1126
//   ue58curve::CycleTime              ↔ Runtime/Engine/Private/Curves/RealCurve.cpp 7–34
//   ue58curve::SolveCubic             ↔ Runtime/Engine/Private/Curves/CurveEvaluation.cpp 10–79
//   ue58curve::BezierToPower          ↔ CurveEvaluation.cpp 81–91
//   ue58curve::WeightedEvalForTwoKeys ↔ CurveEvaluation.cpp 93–191
//   ue58curve::IsItNotWeighted        ↔ CurveEvaluation.cpp 193–197
//   ue58curve::EvalForTwoKeys         ↔ CurveEvaluation.cpp 205–250
//   ue58curve::BezierInterp           ↔ Runtime/Engine/Public/Curves/CurveEvaluation.h 25–36
//   ue58curve::ComputeCurveTangent    ↔ Runtime/Core/Public/Math/InterpCurvePoint.h 198–204
//                                       (AutoCalcTangent)·240–284(clamp 없는 갈래)·289–301
namespace rocks::test {

namespace key_curve_golden {

enum class Interp : uint8 {
    Linear = 0,
    Constant = 1,
    Cubic = 2
};
enum class Weight : uint8 {
    None = 0,
    Arrive = 1,
    Leave = 2,
    Both = 3
};

// FRichCurveKey의 평가에 쓰이는 칸과 같은 칸이다(KeyCurveMath의 CurveSegmentKey 형식).
struct Key {
    float time = 0.0f;
    float value = 0.0f;
    Interp interp = Interp::Linear;
    Weight tangentWeight = Weight::None;
    float arriveTangent = 0.0f;
    float arriveTangentWeight = 0.0f;
    float leaveTangent = 0.0f;
    float leaveTangentWeight = 0.0f;
};

static_assert(math::CurveSegmentKey<Key>);

} // namespace key_curve_golden

namespace ue58curve {

using key_curve_golden::Interp;
using key_curve_golden::Key;
using key_curve_golden::Weight;

// 15절 수정의 스위치다. 기본값(모두 켬)이 Rocks 의미다.
struct Options {
    bool exactLerpEndpoints = true; // 15절 2: Lerp 끝점·끝의 차 넘침
    bool fixCycle = true;           // 15절 1: 길이 0·비유한 시간·int32 밖 주기
    bool fixUnfilledRoot = true;    // 15절 3: 근 2개일 때 세 번째 칸
    // fixUnfilledRoot가 거짓일 때 세 번째 칸의 값(UE는 초기화되지 않은 stack)이다.
    double unfilledRoot = 0.0;
    // 있으면 마지막 Lerp의 비율을 남긴다(Bezier에 실제로 넣은 비율 — 가중 경로는 (float)근).
    float* lastAlpha = nullptr;
};

constexpr float UE_PI = 3.1415926535897932f;
constexpr float UE_INV_PI = 0.31830988618f;
constexpr float UE_HALF_PI = 1.57079632679f;
constexpr float UE_TWO_PI = 6.28318530717f;
constexpr double UE_DOUBLE_PI = 3.141592653589793238462643383279502884197169399;
constexpr double UE_DOUBLE_SMALL_NUMBER = 1.e-8;
constexpr float UE_KINDA_SMALL_NUMBER = 1.e-4f;

// x64 cvtss2si(현재 반올림 방식 = 짝수 쪽 최근접): 범위 밖·NaN은 int32 최솟값(정수 무효값)이다.
inline int32 ConvertSse(float value) noexcept {
    const double wide = value;
    double rounded = platform::PlatformMath::Floor(wide);
    const double fraction = wide - rounded;
    if (fraction > 0.5 || (fraction == 0.5 && platform::PlatformMath::Fmod(rounded, 2.0) != 0.0)) {
        rounded += 1.0;
    }

    if (!(rounded >= -2147483648.0 && rounded <= 2147483647.0)) {
        return NumericLimits<int32>::Min();
    }

    return static_cast<int32>(rounded);
}

// UnrealPlatformMathSSE.h 86–91: _mm_cvt_ss2si(_mm_set_ss(F + F - 0.5f)) >> 1
inline int32 FloorToInt32Sse(float F) noexcept {
    return ConvertSse(F + F - 0.5f) >> 1;
}

// x64 cvttss2si(64비트): 범위 밖·NaN은 int64 최솟값이다.
inline int64 TruncToInt64(float value) noexcept {
    if (value >= -9223372036854775808.0f && value < 9223372036854775808.0f) {
        return static_cast<int64>(value);
    }

    return NumericLimits<int64>::Min();
}

// UnrealMathUtility.h 760–798(T = float)
inline void SinCos(float* ScalarSin, float* ScalarCos, float Value) noexcept {
    // Map Value to y in [-pi,pi], x = 2*pi*quotient + remainder.
    float quotient = (UE_INV_PI * 0.5f) * Value;
    if (Value >= 0.0f) {
        quotient = (float)(TruncToInt64(quotient + 0.5f));
    } else {
        quotient = (float)(TruncToInt64(quotient - 0.5f));
    }

    float y = Value - UE_TWO_PI * quotient;

    // Map y to [-pi/2,pi/2] with sin(y) = sin(Value).
    float sign;
    if (y > UE_HALF_PI) {
        y = UE_PI - y;
        sign = -1.0f;
    } else if (y < -UE_HALF_PI) {
        y = -UE_PI - y;
        sign = -1.0f;
    } else {
        sign = +1.0f;
    }

    float y2 = y * y;

    // 11-degree minimax approximation
    *ScalarSin =
        (((((-2.3889859e-08f * y2 + 2.7525562e-06f) * y2 - 0.00019840874f) * y2 + 0.0083333310f) *
              y2 -
          0.16666667f) *
             y2 +
         1.0f) *
        y;

    // 10-degree minimax approximation
    float p =
        ((((-2.6051615e-07f * y2 + 2.4760495e-05f) * y2 - 0.0013888378f) * y2 + 0.041666638f) * y2 -
         0.5f) *
            y2 +
        1.0f;
    *ScalarCos = sign * p;
}

// UnrealMathUtility.h 1123–1126: (T)(A + Alpha * (B-A)). exactLerpEndpoints면 Rocks math::Lerp다.
inline float Lerp(float A, float B, float Alpha, const Options& options) noexcept {
    if (options.lastAlpha) {
        *options.lastAlpha = Alpha;
    }

    if (options.exactLerpEndpoints) {
        return math::Lerp(A, B, Alpha);
    }

    return (float)(A + Alpha * (B - A));
}

// CurveEvaluation.h 25–36
inline float BezierInterp(float P0, float P1, float P2, float P3, float Alpha,
                          const Options& options) noexcept {
    const float P01 = Lerp(P0, P1, Alpha, options);
    const float P12 = Lerp(P1, P2, Alpha, options);
    const float P23 = Lerp(P2, P3, Alpha, options);
    const float P012 = Lerp(P01, P12, Alpha, options);
    const float P123 = Lerp(P12, P23, Alpha, options);
    const float P0123 = Lerp(P012, P123, Alpha, options);

    return P0123;
}

// UnrealMathUtility.h IsNearlyZero·IsNearlyEqual(double, 기본 허용오차 UE_DOUBLE_SMALL_NUMBER)
inline bool IsNearlyZero(double Value) noexcept {
    return platform::PlatformMath::Abs(Value) <= UE_DOUBLE_SMALL_NUMBER;
}

inline bool IsNearlyEqual(double A, double B) noexcept {
    return platform::PlatformMath::Abs(A - B) <= UE_DOUBLE_SMALL_NUMBER;
}

// CurveEvaluation.cpp 10–79
inline int32 SolveCubic(double Coeff[4], double Solution[3]) noexcept {
    auto Cbrt = [](double x) -> double {
        return ((x) > 0.0
                    ? platform::PlatformMath::Pow((x), 1.0 / 3.0)
                    : ((x) < 0.0 ? -platform::PlatformMath::Pow((double)-(x), 1.0 / 3.0) : 0.0));
    };
    int32 NumSolutions = 0;

    // Normal form: x^3 + Ax^2 + Bx + C = 0
    double Denominator =
        (Coeff[3] != 0.0) ? Coeff[3] : UE_DOUBLE_SMALL_NUMBER * UE_DOUBLE_SMALL_NUMBER;
    double A = Coeff[2] / Denominator;
    double B = Coeff[1] / Denominator;
    double C = Coeff[0] / Denominator;

    // Substitute x = y - A/3 to eliminate quadric term: x^3 +px + q = 0
    double SqOfA = A * A;
    double P = 1.0 / 3 * (-1.0 / 3 * SqOfA + B);
    double Q = 1.0 / 2 * (2.0 / 27 * A * SqOfA - 1.0 / 3 * A * B + C);

    // Use Cardano's formula
    double CubeOfP = P * P * P;
    double D = Q * Q + CubeOfP;
    if (IsNearlyZero(D)) {
        // One triple solution
        if (IsNearlyZero(Q)) {
            Solution[0] = 0;
            NumSolutions = 1;
        }
        // One single and one double solution
        else {
            double U = Cbrt(-Q);
            Solution[0] = 2 * U;
            Solution[1] = -U;
            NumSolutions = 2;
        }
    }
    // Three real solutions
    else if (D < 0) {
        double Phi =
            1.0 / 3 * platform::PlatformMath::Acos(-Q / platform::PlatformMath::Sqrt(-CubeOfP));
        double T = 2 * platform::PlatformMath::Sqrt(-P);

        Solution[0] = T * platform::PlatformMath::Cos(Phi);
        Solution[1] = -T * platform::PlatformMath::Cos(Phi + UE_DOUBLE_PI / 3);
        Solution[2] = -T * platform::PlatformMath::Cos(Phi - UE_DOUBLE_PI / 3);
        NumSolutions = 3;
    }
    // One real solution
    else {
        double SqrtD = platform::PlatformMath::Sqrt(D);
        double U = Cbrt(SqrtD - Q);
        double V = -Cbrt(SqrtD + Q);

        Solution[0] = U + V;
        NumSolutions = 1;
    }

    // Resubstitute
    double Sub = 1.0 / 3 * A;
    for (int32 Index = 0; Index < NumSolutions; ++Index) {
        Solution[Index] -= Sub;
    }

    return NumSolutions;
}

// CurveEvaluation.cpp 81–91
inline void BezierToPower(double A1, double B1, double C1, double D1, double* A2, double* B2,
                          double* C2, double* D2) noexcept {
    double A = B1 - A1;
    double B = C1 - B1;
    double C = D1 - C1;
    double D = B - A;
    *A2 = C - B - D;
    *B2 = 3.0 * D;
    *C2 = 3.0 * A;
    *D2 = A1;
}

// CurveEvaluation.cpp 93–191. 근 2개일 때 UE는 Results[2](초기화되지 않음)까지 읽는다 —
// fixUnfilledRoot가 거짓이면 그 칸에 options.unfilledRoot를 두고 UE처럼 세 칸을 다 본다.
inline float WeightedEvalForTwoKeys(float Key1Value, float Key1Time, float Key1LeaveTangent,
                                    float Key1LeaveTangentWeight, Weight Key1TangentWeightMode,
                                    float Key2Value, float Key2Time, float Key2ArriveTangent,
                                    float Key2ArriveTangentWeight, Weight Key2TangentWeightMode,
                                    float InTime, const Options& options) noexcept {
    const float Diff = Key2Time - Key1Time;
    const float Alpha = (InTime - Key1Time) / Diff;
    const float OneThird = 1.0f / 3.0f;
    const double Time1 = Key1Time;
    const double Time2 = Key2Time;
    const float X = (float)(Time2 - Time1);
    float CosAngle, SinAngle;
    float Angle = platform::PlatformMath::Atan(Key1LeaveTangent);
    SinCos(&SinAngle, &CosAngle, Angle);
    float LeaveWeight;
    if (Key1TangentWeightMode == Weight::None || Key1TangentWeightMode == Weight::Arrive) {
        const float LeaveTangentNormalized = Key1LeaveTangent;
        const float Y = LeaveTangentNormalized * X;
        LeaveWeight = platform::PlatformMath::Sqrt(X * X + Y * Y) * OneThird;
    } else {
        LeaveWeight = Key1LeaveTangentWeight;
    }
    const float Key1TanX = (float)(CosAngle * LeaveWeight + Time1);
    const float Key1TanY = SinAngle * LeaveWeight + Key1Value;

    Angle = platform::PlatformMath::Atan(Key2ArriveTangent);
    SinCos(&SinAngle, &CosAngle, Angle);
    float ArriveWeight;
    if (Key2TangentWeightMode == Weight::None || Key2TangentWeightMode == Weight::Leave) {
        const float ArriveTangentNormalized = Key2ArriveTangent;
        const float Y = ArriveTangentNormalized * X;
        ArriveWeight = platform::PlatformMath::Sqrt(X * X + Y * Y) * OneThird;
    } else {
        ArriveWeight = Key2ArriveTangentWeight;
    }
    const float Key2TanX = (float)(-CosAngle * ArriveWeight + Time2);
    const float Key2TanY = -SinAngle * ArriveWeight + Key2Value;

    // Normalize the Time Range
    const float RangeX = (float)(Time2 - Time1);

    const float Dx1 = (float)(Key1TanX - Time1);
    const float Dx2 = (float)(Key2TanX - Time1);

    // Normalize values
    const float NormalizedX1 = Dx1 / RangeX;
    const float NormalizedX2 = Dx2 / RangeX;

    double Coeff[4];
    double Results[3];

    // Convert Bezier to Power basis, also float to double for precision for root finding.
    BezierToPower(0.0, NormalizedX1, NormalizedX2, 1.0, &(Coeff[3]), &(Coeff[2]), &(Coeff[1]),
                  &(Coeff[0]));

    Coeff[0] = Coeff[0] - Alpha;

    const int32 NumResults = SolveCubic(Coeff, Results);
    if (NumResults == 2) {
        Results[2] = options.unfilledRoot;
    }
    const int32 ReadCount = (options.fixUnfilledRoot ? NumResults : 3);
    double NewInterp = Alpha;
    if (NumResults == 1) {
        NewInterp = Results[0];
    } else {
        NewInterp = NumericLimits<double>::Lowest(); // just need to be out of range
        for (int32 Index = 0; Index < ReadCount; ++Index) {
            const double Result = Results[Index];
            if ((Result > 0.0 || IsNearlyEqual(Result, 0.0)) &&
                (Result < 1.0 || IsNearlyEqual(Result, 1.0))) {
                if (NewInterp < 0.0 || Result > NewInterp) {
                    NewInterp = Result;
                }
            }
        }

        if (NewInterp == NumericLimits<double>::Lowest()) {
            NewInterp = 0.0;
        }
    }
    // now use NewInterp and adjusted tangents plugged into the Y (Value) part of the graph.
    const float P0 = Key1Value;
    const float P1 = Key1TanY;
    const float P3 = Key2Value;
    const float P2 = Key2TanY;

    return BezierInterp(P0, P1, P2, P3, (float)NewInterp, options);
}

// CurveEvaluation.cpp 193–197
inline bool IsItNotWeighted(const Key& Key1, const Key& Key2) noexcept {
    return ((Key1.tangentWeight == Weight::None || Key1.tangentWeight == Weight::Arrive) &&
            (Key2.tangentWeight == Weight::None || Key2.tangentWeight == Weight::Leave));
}

// CurveEvaluation.cpp 205–250
inline float EvalForTwoKeys(const Key& Key1, const Key& Key2, const float InTime,
                            const Options& options) noexcept {
    const float Diff = Key2.time - Key1.time;

    if (Diff > 0.f) {
        if (Key1.interp != Interp::Constant) {
            const float Alpha = (InTime - Key1.time) / Diff;
            const float P0 = Key1.value;
            const float P3 = Key2.value;

            if (Key1.interp == Interp::Linear) {
                return Lerp(P0, P3, Alpha, options);
            } else {
                if (IsItNotWeighted(Key1, Key2)) {
                    const float OneThird = 1.0f / 3.0f;
                    const float P1 = P0 + (Key1.leaveTangent * Diff * OneThird);
                    const float P2 = P3 - (Key2.arriveTangent * Diff * OneThird);

                    return BezierInterp(P0, P1, P2, P3, Alpha, options);
                } else {
                    return WeightedEvalForTwoKeys(
                        Key1.value, Key1.time, Key1.leaveTangent, Key1.leaveTangentWeight,
                        Key1.tangentWeight, Key2.value, Key2.time, Key2.arriveTangent,
                        Key2.arriveTangentWeight, Key2.tangentWeight, InTime, options);
                }
            }
        } else {
            return InTime < Key2.time ? Key1.value : Key2.value;
        }
    } else {
        return Key1.value;
    }
}

// RealCurve.cpp 7–34. CycleCount는 UE int다. fixCycle이면 15절 1의 수정(길이 0·비유한은 재배치
// 없음, 주기 몫은 SSE 식 대신 올바른 floor)을 켠다. 몫이 int32 밖인 사례는 이 참조로 대조하지
// 않는다.
struct CycleResult {
    float time = 0.0f;
    double cycleCount = 0.0;
};

inline CycleResult CycleTime(float MinTime, float MaxTime, float InTime,
                             const Options& options) noexcept {
    float InitTime = InTime;
    float Duration = MaxTime - MinTime;
    int32 CycleCount = 0;

    if (options.fixCycle) {
        if (!(Duration > 0.0f) || !platform::PlatformMath::IsFinite(Duration) ||
            !platform::PlatformMath::IsFinite(InTime)) {
            return {InTime, 0.0};
        }

        // 몫이 int32 밖인 double 재배치(D16)는 참조에 두지 않는다. 그 갈래는
        // MathKeyCurveTests.cpp의 손 계산 벡터로 확인하고, 여기 대조 사례는 몫이 int32 안이다.
        // int32 안: UE 식 그대로에 올바른 floor(설계 5.1 — |몫| ∈ [2^30, 2^31)에서 SSE 식과
        // 다르다).
        if (InTime > MaxTime) {
            CycleCount =
                (int32)platform::PlatformMath::Floor((double)((MaxTime - InTime) / Duration));
            InTime = InTime + Duration * CycleCount;
        } else if (InTime < MinTime) {
            CycleCount =
                (int32)platform::PlatformMath::Floor((double)((InTime - MinTime) / Duration));
            InTime = InTime - Duration * CycleCount;
        }
    } else {
        if (InTime > MaxTime) {
            CycleCount = FloorToInt32Sse((MaxTime - InTime) / Duration);
            InTime = InTime + Duration * CycleCount;
        } else if (InTime < MinTime) {
            CycleCount = FloorToInt32Sse((InTime - MinTime) / Duration);
            InTime = InTime - Duration * CycleCount;
        }
    }

    if (InTime == MaxTime && InitTime < MinTime) {
        InTime = MinTime;
    }

    if (InTime == MinTime && InitTime > MaxTime) {
        InTime = MaxTime;
    }

    // FMath::Abs(int): int32 최솟값은 UE에서 감싸 돈다. 여기서는 double 절댓값이다(UE 비트 대조
    // 범위 밖).
    return {InTime, platform::PlatformMath::Abs((double)CycleCount)};
}

// InterpCurvePoint.h 198–204(AutoCalcTangent<float, float>)·240–284(bWantClamping = false 갈래)
inline float ComputeCurveTangent(float PrevTime, float PrevPoint, float CurTime, float CurPoint,
                                 float NextTime, float NextPoint, float Tension) noexcept {
    (void)CurTime;
    float OutTangent = (1.f - Tension) * ((CurPoint - PrevPoint) + (NextPoint - CurPoint));
    const float PrevToNextTimeDiff = math::Max<float>(UE_KINDA_SMALL_NUMBER, NextTime - PrevTime);
    OutTangent /= PrevToNextTimeDiff;
    return OutTangent;
}

} // namespace ue58curve

} // namespace rocks::test
