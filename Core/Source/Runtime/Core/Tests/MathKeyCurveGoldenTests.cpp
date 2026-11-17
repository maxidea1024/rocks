#include <TestSupport/Test.h>
#include <TestSupport/MathTestSupport.h>
#include <Core/Math/KeyCurveMath.h>
#include <Platform/Memory.h>
#include <Platform/PlatformMath.h>

// KeyCurveMath(Docs/Design/CurveData.ko.md 5·6절)를 UE 5.8 식을 그대로 옮긴 참조 구현과 비트 단위로
// 대조한다. 참조는 UE 원본을 줄 단위로 옮긴 것이고(형식·암묵 변환·연산 순서를 UE 소스 그대로
// 적었다) 설계 문서 15절의 수정을 Options로 하나씩 켜고 끈다.
// - 수정을 모두 켠 참조 = Rocks 의미: 모든 입력에서 비트가 같아야 한다.
// - 수정을 끈 참조 = UE 그대로: 문서화한 경우(비율 0·1, 끝의 차 넘침, 근 2개, |주기 몫| ≥ 2^30,
//   길이 0·비유한 순환)만 다르다.
// 참조의 FloorToInt는 Windows x64 UE의 SSE 식 cvt(F + F − 0.5f) >> 1이고, SinCos는 UE 다항식이다.
// 무작위 사례는 고정 seed라 매 실행이 같다.
//
// 참조 함수 ↔ UE 5.8.4 원본(리뷰어 줄 단위 대조용):
//   ue58::FloorToInt32Sse        ↔ Runtime/Core/Public/Math/UnrealPlatformMathSSE.h 86–91
//   ue58::TruncToInt64           ↔ x64 cvttss2si(64비트) — UnrealMathUtility.h 767·771의 (int64)
//   변환 ue58::SinCos                 ↔ Runtime/Core/Public/Math/UnrealMathUtility.h 760–798
//   ue58::Lerp                   ↔ Runtime/Core/Public/Math/UnrealMathUtility.h 1119–1126
//   ue58::CycleTime              ↔ Runtime/Engine/Private/Curves/RealCurve.cpp 7–34
//   ue58::SolveCubic             ↔ Runtime/Engine/Private/Curves/CurveEvaluation.cpp 10–79
//   ue58::BezierToPower          ↔ CurveEvaluation.cpp 81–91
//   ue58::WeightedEvalForTwoKeys ↔ CurveEvaluation.cpp 93–191
//   ue58::IsItNotWeighted        ↔ CurveEvaluation.cpp 193–197
//   ue58::EvalForTwoKeys         ↔ CurveEvaluation.cpp 205–250
//   ue58::BezierInterp           ↔ Runtime/Engine/Public/Curves/CurveEvaluation.h 25–36
//   ue58::ComputeCurveTangent    ↔ Runtime/Core/Public/Math/InterpCurvePoint.h
//   198–204(AutoCalcTangent)·
//                                  240–284(clamp 없는 갈래)·289–301
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

namespace ue58 {

using key_curve_golden::Interp;
using key_curve_golden::Key;
using key_curve_golden::Weight;

// 15절 수정의 스위치다. 기본값(모두 켬)이 Rocks 의미다.
struct Options {
    bool exactLerpEndpoints = true; // 15절 2: Lerp 끝점·끝의 차 넘침
    bool fixCycle = true;           // 15절 1: 길이 0·비유한 시간·int32 밖 주기
    bool fixUnfilledRoot = true;    // 15절 3: 근 2개일 때 세 번째 칸
    double unfilledRoot =
        0.0; // fixUnfilledRoot가 거짓일 때 세 번째 칸의 값(UE는 초기화되지 않은 stack)
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

} // namespace ue58

namespace key_curve_golden {

template <typename S>
bool SameBits(S a, S b) noexcept {
    if constexpr (IsSameV<S, float>) {
        return platform::BitCast<uint32>(a) == platform::BitCast<uint32>(b);
    } else {
        return platform::BitCast<uint64>(a) == platform::BitCast<uint64>(b);
    }
}

float RandomFloat(MathRandom& random, double minimum, double maximum) noexcept {
    return static_cast<float>(random.Range(minimum, maximum));
}

// 접선: 대부분 [−5, 5], 가끔 큰 값·0.
float RandomTangent(MathRandom& random) noexcept {
    const uint64 pick = random.Next() % 8;
    if (pick == 0) {
        return 0.0f;
    }

    if (pick == 1) {
        return RandomFloat(random, -1000.0, 1000.0);
    }

    return RandomFloat(random, -5.0, 5.0);
}

Key RandomKey(MathRandom& random, float time) noexcept {
    Key key;
    key.time = time;
    key.value = RandomFloat(random, -50.0, 50.0);
    key.interp = static_cast<Interp>(random.Next() % 3);
    key.tangentWeight = static_cast<Weight>(random.Next() % 4);
    key.arriveTangent = RandomTangent(random);
    key.leaveTangent = RandomTangent(random);
    key.arriveTangentWeight = RandomFloat(random, 0.0, 4.0);
    key.leaveTangentWeight = RandomFloat(random, 0.0, 4.0);
    return key;
}

// 한 구간과 평가 시간을 만든다. 구간은 가끔 아주 짧거나 길이 0이고, 시간은 대부분 구간 안이다.
struct Segment {
    Key first;
    Key second;
    float time = 0.0f;
};

Segment RandomSegment(MathRandom& random) noexcept {
    const float start = RandomFloat(random, -100.0, 100.0);
    const uint64 lengthPick = random.Next() % 10;
    float length;
    if (lengthPick == 0) {
        length = 0.0f;
    } else if (lengthPick == 1) {
        length = RandomFloat(random, 1e-5, 1e-3);
    } else {
        length = RandomFloat(random, 0.01, 20.0);
    }

    Segment segment;
    segment.first = RandomKey(random, start);
    segment.second = RandomKey(random, start + length);
    const uint64 timePick = random.Next() % 10;
    if (timePick == 0) {
        segment.time = segment.first.time;
    } else if (timePick == 1) {
        segment.time = segment.second.time;
    } else if (timePick == 2) {
        segment.time = start + RandomFloat(random, -5.0, 25.0);
    } else {
        segment.time = start + static_cast<float>(random.Unit()) * length;
    }

    return segment;
}

// 같은 입력에서 Rocks 결과와 참조 결과의 비트가 다른 사례 수다. include가 거짓인 사례는 세지
// 않는다.
template <typename Include>
int32 CountSegmentMismatches(uint64 seed, int32 cases, const ue58::Options& options,
                             Include include) noexcept {
    MathRandom random(seed);
    int32 mismatches = 0;
    for (int32 index = 0; index < cases; ++index) {
        const Segment segment = RandomSegment(random);
        if (!include(segment)) {
            continue;
        }

        const float rocks = math::EvalCurveSegment(segment.first, segment.second, segment.time);
        const float reference =
            ue58::EvalForTwoKeys(segment.first, segment.second, segment.time, options);
        if (!SameBits(rocks, reference)) {
            ++mismatches;
        }
    }

    return mismatches;
}

bool AnySegment(const Segment&) noexcept {
    return true;
}

// UE 그대로와 비트가 같아야 하는 사례인가: 비율이 0·1이 아니고, 길이가 0이 아니며, 끝의 차가 넘치지
// 않는다. 근 2개 가중 구간은 무작위로는 거의 나오지 않고 따로 시험한다.
bool InsideUeBitRange(const Segment& segment) noexcept {
    const float difference = segment.second.time - segment.first.time;
    if (!(difference > 0.0f)) {
        return false;
    }

    if (segment.first.interp == Interp::Constant) {
        return true;
    }

    const float alpha = (segment.time - segment.first.time) / difference;
    return alpha != 0.0f && alpha != 1.0f;
}

} // namespace key_curve_golden

using namespace key_curve_golden;

TEST(KeyCurveGolden_SegmentMatchesUeFormulasBitExactly) {
    const ue58::Options rocksOptions;
    CHECK(CountSegmentMismatches(0x3001, 20000, rocksOptions, AnySegment) == 0);

    // 모든 구간을 가중 Cubic으로 만들어 가중 경로를 많이 지나게 한다.
    MathRandom random(0x3002);
    int32 mismatches = 0;
    int32 threeRootCases = 0;
    for (int32 index = 0; index < 20000; ++index) {
        Segment segment = RandomSegment(random);
        segment.first.interp = Interp::Cubic;
        segment.first.tangentWeight = (index & 1) ? Weight::Both : Weight::Leave;
        segment.second.tangentWeight = (index & 2) ? Weight::Both : Weight::Arrive;
        const float rocks = math::EvalCurveSegment(segment.first, segment.second, segment.time);
        const float reference =
            ue58::EvalForTwoKeys(segment.first, segment.second, segment.time, rocksOptions);
        if (!SameBits(rocks, reference)) {
            ++mismatches;
        }

        // 시간 Bezier가 단조가 아니면(근 3개) 근 고르기 갈래를 지난다.
        const float n1 = segment.first.leaveTangentWeight;
        if (n1 > (segment.second.time - segment.first.time)) {
            ++threeRootCases;
        }
    }

    CHECK(mismatches == 0);
    CHECK(threeRootCases > 0);
}

TEST(KeyCurveGolden_UeLiteralDiffersOnlyInDocumentedCases) {
    ue58::Options literal;
    literal.exactLerpEndpoints = false;
    literal.fixUnfilledRoot = false;
    literal.unfilledRoot = 0.0;
    CHECK(CountSegmentMismatches(0x3101, 20000, literal, InsideUeBitRange) == 0);

    // 비율 0·1에서는 Lerp 끝점이 달라 UE 그대로와 갈리는 사례가 있다(15절 2).
    CHECK(CountSegmentMismatches(0x3102, 20000, literal, AnySegment) > 0);

    // 반례: 끝의 차가 넘치는 Linear 구간에서 UE는 Inf, Rocks는 유한하다.
    Segment overflow;
    overflow.first.time = 0.0f;
    overflow.first.value = -3.0e38f;
    overflow.second.time = 1.0f;
    overflow.second.value = 3.0e38f;
    const float ueOverflow = ue58::EvalForTwoKeys(overflow.first, overflow.second, 0.5f, literal);
    const float rocksOverflow = math::EvalCurveSegment(overflow.first, overflow.second, 0.5f);
    CHECK(!platform::PlatformMath::IsFinite(ueOverflow));
    CHECK(platform::PlatformMath::IsFinite(rocksOverflow) && rocksOverflow == 0.0f);
}

TEST(KeyCurveGolden_SinCosMatchesUePolynomial) {
    MathRandom random(0x3201);
    int32 mismatches = 0;
    for (int32 index = 0; index < 200000; ++index) {
        float angle;
        const uint64 pick = index % 4;
        if (pick == 0) {
            angle = static_cast<float>(random.Range(-1.5707964, 1.5707964));
        } else if (pick == 1) {
            angle = static_cast<float>(random.Range(-10.0, 10.0));
        } else if (pick == 2) {
            angle = static_cast<float>(random.Range(-1.0e6, 1.0e6));
        } else {
            angle = platform::PlatformMath::Atan(static_cast<float>(random.Range(-1.0e4, 1.0e4)));
        }

        float sine;
        float cosine;
        math::CurveSinCos(angle, sine, cosine);
        float ueSine;
        float ueCosine;
        ue58::SinCos(&ueSine, &ueCosine, angle);
        if (!SameBits(sine, ueSine) || !SameBits(cosine, ueCosine)) {
            ++mismatches;
        }
    }

    CHECK(mismatches == 0);

    // NaN·±Inf·아주 큰 값도 x64 정수 무효값 규칙까지 같다.
    const float specials[] = {platform::PlatformMath::QuietNaN<float>(),
                              platform::PlatformMath::Infinity<float>(),
                              -platform::PlatformMath::Infinity<float>(),
                              1.0e30f,
                              -1.0e30f,
                              0.0f,
                              -0.0f};
    for (const float angle : specials) {
        float sine;
        float cosine;
        math::CurveSinCos(angle, sine, cosine);
        float ueSine;
        float ueCosine;
        ue58::SinCos(&ueSine, &ueCosine, angle);
        CHECK(SameBits(sine, ueSine) && SameBits(cosine, ueCosine));
    }
}

TEST(KeyCurveGolden_SolveCubicMatchesUe) {
    MathRandom random(0x3301);
    int32 mismatches = 0;
    int32 counts[4] = {0, 0, 0, 0};
    for (int32 index = 0; index < 100000; ++index) {
        double coefficients[4];
        for (double& coefficient : coefficients) {
            coefficient = random.Range(-10.0, 10.0);
        }

        if (index % 10 == 0) {
            coefficients[3] = 0.0;
        }

        // (x − r1)(x − r2)(x − r3)로 근 3개 사례를 늘린다.
        if (index % 3 == 1) {
            const double r1 = random.Range(-2.0, 2.0);
            const double r2 = random.Range(-2.0, 2.0);
            const double r3 = random.Range(-2.0, 2.0);
            coefficients[3] = 1.0;
            coefficients[2] = -(r1 + r2 + r3);
            coefficients[1] = r1 * r2 + r2 * r3 + r1 * r3;
            coefficients[0] = -(r1 * r2 * r3);
        }

        const math::CurveCubicRoots rocks = math::SolveCurveCubic(coefficients);
        double ueCoefficients[4] = {coefficients[0], coefficients[1], coefficients[2],
                                    coefficients[3]};
        double ueRoots[3] = {0.0, 0.0, 0.0};
        const int32 ueCount = ue58::SolveCubic(ueCoefficients, ueRoots);
        bool same = static_cast<int32>(rocks.count) == ueCount;
        for (int32 root = 0; same && root < ueCount; ++root) {
            same = SameBits(rocks.roots[root], ueRoots[root]);
        }

        if (!same) {
            ++mismatches;
        }

        if (ueCount >= 1 && ueCount <= 3) {
            ++counts[ueCount];
        }
    }

    CHECK(mismatches == 0);
    CHECK(counts[1] > 0 && counts[3] > 0);

    // 중근: (x − 1)²(x + 2) = x³ − 3x + 2 → 근 {−2, 1}(2개).
    const double doubleRoot[4] = {2.0, -3.0, 0.0, 1.0};
    const math::CurveCubicRoots solved = math::SolveCurveCubic(doubleRoot);
    CHECK(solved.count == 2);
    CHECK(solved.roots[0] == -2.0 && solved.roots[1] == 1.0 && solved.roots[2] == 0.0);
}

TEST(KeyCurveGolden_CycleTimeMatchesUeInsideSseRange) {
    const ue58::Options fixed;
    ue58::Options literal;
    literal.fixCycle = false;
    MathRandom random(0x3401);
    int32 fixedMismatches = 0;
    int32 literalMismatches = 0;
    for (int32 index = 0; index < 100000; ++index) {
        const float minimum = static_cast<float>(random.Range(-100.0, 100.0));
        const float duration =
            static_cast<float>(index % 5 == 0 ? random.Range(1e-4, 1e-2) : random.Range(0.1, 50.0));
        const float maximum = minimum + duration;
        float time;
        const uint64 pick = random.Next() % 4;
        if (pick == 0) {
            time = minimum - static_cast<float>(random.Range(0.0, 1.0e4));
        } else if (pick == 1) {
            time = maximum + static_cast<float>(random.Range(0.0, 1.0e4));
        } else if (pick == 2) {
            // 정확한 배수 근처(주기 경계).
            time = maximum + (maximum - minimum) * static_cast<float>(random.Next() % 100);
        } else {
            time = minimum - (maximum - minimum) * static_cast<float>(random.Next() % 100);
        }

        const math::CurveCycle rocks = math::CycleCurveTime(minimum, maximum, time);
        const ue58::CycleResult reference = ue58::CycleTime(minimum, maximum, time, fixed);
        if (!SameBits(rocks.time, reference.time) || rocks.cycleCount != reference.cycleCount ||
            rocks.wide) {
            ++fixedMismatches;
        }

        // 길이가 0보다 크고 |몫| < 2^30이면 UE SSE 식 그대로와도 같다.
        if (maximum > minimum) {
            const ue58::CycleResult ue = ue58::CycleTime(minimum, maximum, time, literal);
            if (!SameBits(rocks.time, ue.time) || rocks.cycleCount != ue.cycleCount) {
                ++literalMismatches;
            }
        }
    }

    CHECK(fixedMismatches == 0);
    CHECK(literalMismatches == 0);
}

TEST(KeyCurveGolden_CycleTimeUeDefectsAreFixed) {
    ue58::Options literal;
    literal.fixCycle = false;

    // 몫 −(2^30 + 128): UE SSE FloorToInt는 2F가 int32를 넘어 −2^30을 내고 시간이 범위 밖(−128)에
    // 남는다.
    const float before = -(1073741824.0f + 128.0f);
    const ue58::CycleResult ueBefore = ue58::CycleTime(0.0f, 1.0f, before, literal);
    CHECK(ueBefore.time == -128.0f && ueBefore.cycleCount == 1073741824.0);
    const math::CurveCycle rocksBefore = math::CycleCurveTime(0.0f, 1.0f, before);
    CHECK(rocksBefore.time == 0.0f && rocksBefore.cycleCount == 1073741952.0 && !rocksBefore.wide);

    // 뒤쪽 정수 시간 2^30 + 128(주기 1의 정확한 배수): Rocks는 max, UE는 128.
    const float after = 1073741824.0f + 128.0f;
    const ue58::CycleResult ueAfter = ue58::CycleTime(0.0f, 1.0f, after, literal);
    CHECK(ueAfter.time == 128.0f);
    const math::CurveCycle rocksAfter = math::CycleCurveTime(0.0f, 1.0f, after);
    CHECK(rocksAfter.time == 1.0f && rocksAfter.cycleCount == 1073741952.0);

    // 길이 0: UE는 0 나눗셈(−Inf 몫 → 정수 무효값 → 주기 −2^30)이다. 시간은 5 + 0·(−2^30) = 5로
    // 남지만 주기 수 2^30이 CycleWithOffset 오프셋을 망가뜨린다. Rocks는 옮기지 않고 주기 0이다.
    const ue58::CycleResult ueZero = ue58::CycleTime(2.0f, 2.0f, 5.0f, literal);
    CHECK(ueZero.time == 5.0f && ueZero.cycleCount == 1073741824.0);
    const math::CurveCycle rocksZero = math::CycleCurveTime(2.0f, 2.0f, 5.0f);
    CHECK(rocksZero.time == 5.0f && rocksZero.cycleCount == 0.0);
}

TEST(KeyCurveGolden_WeightedTwoRootsDoesNotReadUnfilledSlot) {
    // 시간 Bezier(0, 1.5, −0.5, 1)는 s ≈ 0.311에서 극대 x ≈ 0.5945다. α를 그 극댓값에 맞추면
    // 판별식이 1e-8 안이라 근이 2개(중근 + 단근)다. 키: (0, 0)·(1, 1), 접선 0, 가중치 1.5(Both) —
    // atan(0) = 0이고 CurveSinCos(0) = (0, 1)이라 제어점 시간이 정확히 1.5와 −0.5다.
    Key first;
    first.time = 0.0f;
    first.value = 0.0f;
    first.interp = Interp::Cubic;
    first.tangentWeight = Weight::Both;
    first.leaveTangentWeight = 1.5f;
    Key second;
    second.time = 1.0f;
    second.value = 1.0f;
    second.interp = Interp::Cubic;
    second.tangentWeight = Weight::Both;
    second.arriveTangentWeight = 1.5f;

    // x(s) = 7s³ − 10.5s² + 4.5s, x′(s) = 0 → s = (21 − √63)/42.
    const double peak = (21.0 - platform::PlatformMath::Sqrt(63.0)) / 42.0;
    const double peakValue = ((7.0 * peak - 10.5) * peak + 4.5) * peak;
    float time = static_cast<float>(peakValue);
    bool found = false;
    for (int32 step = 0; step < 64 && !found; ++step) {
        double coefficients[4] = {-static_cast<double>(time), 4.5, -10.5, 7.0};
        double roots[3] = {0.0, 0.0, 0.0};
        if (ue58::SolveCubic(coefficients, roots) == 2) {
            found = true;
            break;
        }

        time = platform::BitCast<float>(platform::BitCast<uint32>(time) + 1u); // 다음 float(양수)
    }

    REQUIRE(found);
    const ue58::Options fixed;
    const float rocks = math::EvalCurveSegment(first, second, time);
    CHECK(SameBits(rocks, ue58::EvalForTwoKeys(first, second, time, fixed)));

    // UE는 채워지지 않은 세 번째 칸까지 본다. 그 칸이 [0, 1] 안의 더 큰 값이면 UE는 그것을 고른다.
    ue58::Options literal;
    literal.fixUnfilledRoot = false;
    literal.unfilledRoot = 0.999;
    const float ue = ue58::EvalForTwoKeys(first, second, time, literal);
    CHECK(!SameBits(rocks, ue));
}

TEST(KeyCurveGolden_WeightedDoubleTimeEqualsFloatTime) {
    // 가중 경로의 시간 칸은 UE처럼 double로 계산한 뒤 float로 저장한다(CE 101–102). 그 식은 모두
    // float 값 둘의 연산 한 번이라(c·w + t1, t2 − t1, tanX − t1) double로 계산해 float로 반올림한
    // 결과가 float 연산 결과와 늘 같다(double 53비트 ≥ 2·24 + 2 — 이중 반올림이 무해한 조건). 설계
    // 개정 2의 "float로만 계산하면 드물게 비트가 다르다"는 성립하지 않으므로, 큰 시간·짧은 구간을
    // 포함한 무작위 입력에서 두 계산이 같고 Rocks가 UE와 같은지 확인한다.
    const auto floatOnly = [](const Key& k1, const Key& k2, float time) noexcept {
        const float diff = k2.time - k1.time;
        const float alpha = (time - k1.time) / diff;
        const float x = k2.time - k1.time;
        float s;
        float c;
        math::CurveSinCos(platform::PlatformMath::Atan(k1.leaveTangent), s, c);
        const float leaveWeight = k1.leaveTangentWeight;
        const float tan1x = c * leaveWeight + k1.time;
        const float tan1y = s * leaveWeight + k1.value;
        math::CurveSinCos(platform::PlatformMath::Atan(k2.arriveTangent), s, c);
        const float arriveWeight = k2.arriveTangentWeight;
        const float tan2x = -c * arriveWeight + k2.time;
        const float tan2y = -s * arriveWeight + k2.value;
        const float n1 = (tan1x - k1.time) / x;
        const float n2 = (tan2x - k1.time) / x;
        const double a = static_cast<double>(n1);
        const double b = static_cast<double>(n2) - static_cast<double>(n1);
        const double cc = 1.0 - static_cast<double>(n2);
        const double d = b - a;
        double coefficients[4] = {0.0 - static_cast<double>(alpha), 3.0 * a, 3.0 * d, cc - b - d};
        double roots[3] = {0.0, 0.0, 0.0};
        const int32 count = ue58::SolveCubic(coefficients, roots);
        double parameter = roots[0];
        if (count != 1) {
            parameter = NumericLimits<double>::Lowest();
            for (int32 index = 0; index < count; ++index) {
                const double root = roots[index];
                if ((root > 0.0 || ue58::IsNearlyEqual(root, 0.0)) &&
                    (root < 1.0 || ue58::IsNearlyEqual(root, 1.0)) &&
                    (parameter < 0.0 || root > parameter)) {
                    parameter = root;
                }
            }

            if (parameter == NumericLimits<double>::Lowest()) {
                parameter = 0.0;
            }
        }

        return math::BezierCurveValue(k1.value, tan1y, tan2y, k2.value,
                                      static_cast<float>(parameter));
    };

    MathRandom random(0x3501);
    const ue58::Options fixed;
    int32 differing = 0;
    int32 mismatches = 0;
    for (int32 index = 0; index < 50000; ++index) {
        Key first;
        first.time = static_cast<float>(index % 2 == 0 ? random.Range(1000.0, 5000.0)
                                                       : random.Range(-1.0e7, 1.0e7));
        first.value = static_cast<float>(random.Range(-10.0, 10.0));
        first.interp = Interp::Cubic;
        first.tangentWeight = Weight::Both;
        first.leaveTangent = static_cast<float>(random.Range(-3.0, 3.0));
        first.leaveTangentWeight = static_cast<float>(random.Range(0.01, 0.3));
        Key second = first;
        second.time = first.time + static_cast<float>(index % 3 == 0 ? random.Range(1.0e-3, 1.0)
                                                                     : random.Range(1.0, 100.0));
        second.value = static_cast<float>(random.Range(-10.0, 10.0));
        second.arriveTangent = static_cast<float>(random.Range(-3.0, 3.0));
        second.arriveTangentWeight = static_cast<float>(random.Range(0.01, 0.3));
        if (!(second.time > first.time)) {
            continue;
        }

        const float time =
            first.time + static_cast<float>(random.Unit()) * (second.time - first.time);
        const float rocks = math::EvalCurveSegment(first, second, time);
        if (!SameBits(rocks, ue58::EvalForTwoKeys(first, second, time, fixed))) {
            ++mismatches;
        }

        if (!SameBits(rocks, floatOnly(first, second, time))) {
            ++differing;
        }
    }

    CHECK(mismatches == 0);
    CHECK(differing == 0);
}

TEST(KeyCurveGolden_AutoTangentMatchesUe) {
    MathRandom random(0x3601);
    int32 mismatches = 0;
    for (int32 index = 0; index < 100000; ++index) {
        const float previousTime = static_cast<float>(random.Range(-100.0, 100.0));
        const float currentTime = previousTime + static_cast<float>(random.Range(0.0, 10.0));
        const float nextTime = index % 50 == 0
                                   ? previousTime
                                   : currentTime + static_cast<float>(random.Range(0.0, 10.0));
        const float previousValue = static_cast<float>(random.Range(-100.0, 100.0));
        const float currentValue = static_cast<float>(random.Range(-100.0, 100.0));
        const float nextValue = static_cast<float>(random.Range(-100.0, 100.0));
        const float tension = static_cast<float>(random.Range(-1.0, 1.0));
        const float rocks = math::ComputeCurveTangent(previousTime, previousValue, currentValue,
                                                      nextTime, nextValue, tension);
        const float ue = ue58::ComputeCurveTangent(previousTime, previousValue, currentTime,
                                                   currentValue, nextTime, nextValue, tension);
        if (!SameBits(rocks, ue)) {
            ++mismatches;
        }
    }

    CHECK(mismatches == 0);
}

} // namespace rocks::test
