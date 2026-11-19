#include <TestSupport/Test.h>
#include <TestSupport/MathTestSupport.h>
#include <Core/Math/InterpCurve.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Memory.h>

// InterpCurve를 두 기준과 대조한다.
// 1. UE 5.8 식을 그대로 옮긴 참조 구현(ue58::Curve). 키·구간 길이·비율은 float, 성분은 값 형식의
//    정밀도, 기저 계수는 float(UE CubicInterp<T, float>), 벡터·색 나눗셈은 역수 곱이다. 설계 문서
//    4절의 차이(Linear 접선 단위, Lerp 끝점)는 선택지로 켜고 끈다. Rocks와 차이를 켠 참조는 모든
//    형식에서 비트 단위로 같아야 하고, 차이를 끈 참조(UE 그대로)와는 문서화한 경우에만 다르다. UE
//    비트 대조의 실제 범위: float·double·Vector2/4(float·double)·Vector3(float)·LinearColor의 일반
//    템플릿 식. UE FVector(Vector3 double)는 CubicInterp SIMD 특수화(항 묶음·FMA)를 써 비트를 맞출
//    수 없고, Vector2·Vector3의 CurveAutoClamped는 UE 결함(double을 float로 읽음, float 벡터는
//    clamp 없음) 이라 UE 그대로와는 대조하지 않는다. quaternion은 UE 대조가 없다(독립 double SQUAD
//    참조).
// 2. 독립 double reference(멱급수 계수로 평가하는 Hermite, 축-각으로 만든 SQUAD, 조밀 표본 최근접).
// 무작위 사례는 고정 seed라 매 실행이 같다.
namespace rocks::test {

namespace ue58 {

// UE 5.8 Math/InterpCurve.h·InterpCurvePoint.h·UnrealMath.cpp(ClampFloatTangent)의 식을 성분 배열로
// 옮긴 것이다.
enum Mode : int32 {
    CIM_Linear = 0,
    CIM_CurveAuto = 1,
    CIM_Constant = 2,
    CIM_CurveUser = 3,
    CIM_CurveBreak = 4,
    CIM_CurveAutoClamped = 5,
};

struct Options {
    // 4절 1: Linear 키 자동 접선을 값 차 대신 기울기로 저장한다.
    bool linearTangentIsSlope = true;
    // 4절 6: Linear 보간의 비율 0·1에서 끝 값을 그대로 쓴다(math::Lerp). UE는 A + Alpha·(B − A).
    bool exactLerpEndpoints = true;
    // CurveAutoClamped를 clamp한다(UE float 커브. Rocks는 Vector2/3·double도).
    bool clampTangents = true;
    // float로 나눌 때 역수를 곱한다(UE TVector·TVector2·TVector4·FLinearColor의 operator/ 와 /=).
    bool reciprocalDivision = false;
};

constexpr int32 MaxPoints = 16;

template <typename S, usize N>
struct Point {
    float InVal = 0.0f;
    S OutVal[N] = {};
    S ArriveTangent[N] = {};
    S LeaveTangent[N] = {};
    int32 InterpMode = CIM_Linear;

    bool IsCurveKey() const noexcept {
        return InterpMode == CIM_CurveAuto || InterpMode == CIM_CurveAutoClamped ||
               InterpMode == CIM_CurveUser || InterpMode == CIM_CurveBreak;
    }
};

template <typename S>
S FMathMax(S a, S b) noexcept {
    return (a >= b) ? a : b;
}

template <typename S>
S FMathMin(S a, S b) noexcept {
    return (a <= b) ? a : b;
}

template <typename S>
S UeLerp(S a, S b, S alpha) noexcept {
    return a + alpha * (b - a);
}

// FMath::CubicInterp<T, float>: 기저 계수는 float, 각 항은 T로 넓혀 곱한다.
template <typename S>
S CubicInterp(S P0, S T0, S P1, S T1, float A) noexcept {
    const float A2 = A * A;
    const float A3 = A2 * A;
    return (S((2 * A3) - (3 * A2) + 1) * P0) + (S(A3 - (2 * A2) + A) * T0) + (S(A3 - A2) * T1) +
           (S((-2 * A3) + (3 * A2)) * P1);
}

template <typename S>
S CubicInterpDerivative(S P0, S T0, S P1, S T1, float A) noexcept {
    const S a = S(6) * P0 + S(3) * T0 + S(3) * T1 - S(6) * P1;
    const S b = S(-6) * P0 - S(4) * T0 - S(2) * T1 + S(6) * P1;
    const S c = T0;
    const float A2 = A * A;
    return (a * S(A2)) + (b * S(A)) + c;
}

template <typename S>
S CubicInterpSecondDerivative(S P0, S T0, S P1, S T1, float A) noexcept {
    const S a = S(12) * P0 + S(6) * T0 + S(6) * T1 - S(12) * P1;
    const S b = S(-6) * P0 - S(4) * T0 - S(2) * T1 + S(6) * P1;
    return (a * S(A)) + b;
}

// ClampFloatTangent. UE는 float 전용이며 S = float에서 원본과 같다.
template <typename S>
S ClampFloatTangent(S PrevPointVal, float PrevTime, S CurPointVal, float CurTime, S NextPointVal,
                    float NextTime) noexcept {
    const float PrevToNextTimeDiff = FMathMax<float>(1.e-4f, NextTime - PrevTime);
    const float PrevToCurTimeDiff = FMathMax<float>(1.e-4f, CurTime - PrevTime);
    const float CurToNextTimeDiff = FMathMax<float>(1.e-4f, NextTime - CurTime);
    S OutTangentVal = 0;
    const S PrevToNextHeightDiff = NextPointVal - PrevPointVal;
    const S PrevToCurHeightDiff = CurPointVal - PrevPointVal;
    const S CurToNextHeightDiff = NextPointVal - CurPointVal;
    if ((PrevToCurHeightDiff >= 0 && CurToNextHeightDiff <= 0) ||
        (PrevToCurHeightDiff <= 0 && CurToNextHeightDiff >= 0)) {
        OutTangentVal = 0;
    } else {
        const S ClampThreshold = S(0.333);
        const S CurToNextTangent = CurToNextHeightDiff / S(CurToNextTimeDiff);
        const S PrevToCurTangent = PrevToCurHeightDiff / S(PrevToCurTimeDiff);
        const S PrevToNextTangent = PrevToNextHeightDiff / S(PrevToNextTimeDiff);
        const S UnclampedTangent = PrevToNextTangent;
        S ClampedTangent = UnclampedTangent;
        const S LowerClampThreshold = ClampThreshold;
        const S UpperClampThreshold = S(1) - ClampThreshold;
        const S CurHeightAlpha = PrevToCurHeightDiff / PrevToNextHeightDiff;
        if (PrevToNextHeightDiff > 0) {
            if (CurHeightAlpha < LowerClampThreshold) {
                const S ClampAlpha = S(1) - CurHeightAlpha / ClampThreshold;
                const S LowerClamp = UeLerp(PrevToNextTangent, PrevToCurTangent, ClampAlpha);
                ClampedTangent = FMathMin(ClampedTangent, LowerClamp);
            }

            if (CurHeightAlpha > UpperClampThreshold) {
                const S ClampAlpha = (CurHeightAlpha - UpperClampThreshold) / ClampThreshold;
                const S UpperClamp = UeLerp(PrevToNextTangent, CurToNextTangent, ClampAlpha);
                ClampedTangent = FMathMin(ClampedTangent, UpperClamp);
            }
        } else {
            if (CurHeightAlpha < LowerClampThreshold) {
                const S ClampAlpha = S(1) - CurHeightAlpha / ClampThreshold;
                const S LowerClamp = UeLerp(PrevToNextTangent, PrevToCurTangent, ClampAlpha);
                ClampedTangent = FMathMax(ClampedTangent, LowerClamp);
            }

            if (CurHeightAlpha > UpperClampThreshold) {
                const S ClampAlpha = (CurHeightAlpha - UpperClampThreshold) / ClampThreshold;
                const S UpperClamp = UeLerp(PrevToNextTangent, CurToNextTangent, ClampAlpha);
                ClampedTangent = FMathMax(ClampedTangent, UpperClamp);
            }
        }

        OutTangentVal = ClampedTangent;
    }

    return OutTangentVal;
}

template <typename S, usize N>
struct Curve {
    Point<S, N> Points[MaxPoints] = {};
    int32 Num = 0;
    bool bIsLooped = false;
    float LoopKeyOffset = 0.0f;
    Options options;

    // 벡터 operator/(Scale)·/=: 역수 RScale = T(1) / Scale를 곱한다. scalar는 그대로 나눈다.
    S Divide(S value, float divisor) const noexcept {
        if (options.reciprocalDivision) {
            const S reciprocal = S(1) / S(divisor);
            return value * reciprocal;
        }

        return value / S(divisor);
    }

    // UE AddPoint: InVal보다 작은 키를 지나 그 앞에 넣는다.
    int32 AddPoint(const Point<S, N>& point) noexcept {
        int32 i = 0;
        for (i = 0; i < Num && Points[i].InVal < point.InVal; i++) {
        }

        for (int32 j = Num; j > i; --j) {
            Points[j] = Points[j - 1];
        }

        Points[i] = point;
        ++Num;
        return i;
    }

    void SetLoopKey(float InLoopKey) noexcept {
        if (Num == 0) {
            bIsLooped = false;
            return;
        }

        const float LastInKey = Points[Num - 1].InVal;
        if (InLoopKey > LastInKey) {
            bIsLooped = true;
            LoopKeyOffset = InLoopKey - LastInKey;
        } else {
            bIsLooped = false;
        }
    }

    int32 GetPointIndexForInputValue(float InValue) const noexcept {
        const int32 NumPoints = Num;
        const int32 LastPoint = NumPoints - 1;
        if (InValue < Points[0].InVal) {
            return -1;
        }

        if (InValue >= Points[LastPoint].InVal) {
            return LastPoint;
        }

        int32 MinIndex = 0;
        int32 MaxIndex = NumPoints;
        while (MaxIndex - MinIndex > 1) {
            const int32 MidIndex = (MinIndex + MaxIndex) / 2;
            if (Points[MidIndex].InVal <= InValue) {
                MinIndex = MidIndex;
            } else {
                MaxIndex = MidIndex;
            }
        }

        return MinIndex;
    }

    S Lerp(S a, S b, float alpha) const noexcept {
        if (options.exactLerpEndpoints && alpha == 0.0f) {
            return a;
        }

        if (options.exactLerpEndpoints && alpha == 1.0f) {
            return b;
        }

        return UeLerp(a, b, S(alpha));
    }

    // 0 = Eval, 1 = EvalDerivative, 2 = EvalSecondDerivative. 빈 커브·NaN은 시험이 넘기지 않는다.
    void Evaluate(int32 order, float InVal, S (&Out)[N]) const noexcept {
        const int32 NumPoints = Num;
        const int32 LastPoint = NumPoints - 1;
        const int32 Index = GetPointIndexForInputValue(InVal);
        if (Index == -1) {
            for (usize c = 0; c < N; ++c) {
                Out[c] = order == 0 ? Points[0].OutVal[c]
                                    : (order == 1 ? Points[0].LeaveTangent[c] : S(0));
            }

            return;
        }

        if (Index == LastPoint) {
            const bool pastEnd = !bIsLooped || InVal >= Points[LastPoint].InVal + LoopKeyOffset;
            if (pastEnd) {
                const Point<S, N>& held = bIsLooped ? Points[0] : Points[LastPoint];
                for (usize c = 0; c < N; ++c) {
                    Out[c] =
                        order == 0 ? held.OutVal[c] : (order == 1 ? held.ArriveTangent[c] : S(0));
                }

                return;
            }
        }

        const bool bLoopSegment = (bIsLooped && Index == LastPoint);
        const int32 NextIndex = bLoopSegment ? 0 : (Index + 1);
        const Point<S, N>& PrevPoint = Points[Index];
        const Point<S, N>& NextPoint = Points[NextIndex];
        const float Diff = bLoopSegment ? LoopKeyOffset : (NextPoint.InVal - PrevPoint.InVal);
        if (Diff > 0.0f && PrevPoint.InterpMode != CIM_Constant) {
            float Alpha = (InVal - PrevPoint.InVal) / Diff;
            // Rocks는 루프 끝의 반올림으로 1을 넘는 비율을 1로 자른다(UE는 checkf).
            Alpha = Alpha > 1.0f ? 1.0f : Alpha;
            for (usize c = 0; c < N; ++c) {
                const S T0 = PrevPoint.LeaveTangent[c] * S(Diff);
                const S T1 = NextPoint.ArriveTangent[c] * S(Diff);
                if (PrevPoint.InterpMode == CIM_Linear) {
                    if (order == 0) {
                        Out[c] = Lerp(PrevPoint.OutVal[c], NextPoint.OutVal[c], Alpha);
                    } else if (order == 1) {
                        Out[c] = Divide(NextPoint.OutVal[c] - PrevPoint.OutVal[c], Diff);
                    } else {
                        Out[c] = S(0);
                    }
                } else if (order == 0) {
                    Out[c] = CubicInterp(PrevPoint.OutVal[c], T0, NextPoint.OutVal[c], T1, Alpha);
                } else if (order == 1) {
                    Out[c] = Divide(CubicInterpDerivative(PrevPoint.OutVal[c], T0,
                                                          NextPoint.OutVal[c], T1, Alpha),
                                    Diff);
                } else {
                    Out[c] = Divide(CubicInterpSecondDerivative(PrevPoint.OutVal[c], T0,
                                                                NextPoint.OutVal[c], T1, Alpha),
                                    Diff * Diff);
                }
            }

            return;
        }

        for (usize c = 0; c < N; ++c) {
            Out[c] = order == 0 ? Points[Index].OutVal[c] : S(0);
        }
    }

    void ComputeCurveTangent(float PrevTime, const S (&PrevPoint)[N], float CurTime,
                             const S (&CurPoint)[N], float NextTime, const S (&NextPoint)[N],
                             float Tension, bool bWantClamping, S (&OutTangent)[N]) const noexcept {
        if (bWantClamping && options.clampTangents) {
            for (usize c = 0; c < N; ++c) {
                const S ClampedTangent = ClampFloatTangent(PrevPoint[c], PrevTime, CurPoint[c],
                                                           CurTime, NextPoint[c], NextTime);
                OutTangent[c] = S(1.0f - Tension) * ClampedTangent;
            }

            return;
        }

        const float PrevToNextTimeDiff = FMathMax<float>(1.e-4f, NextTime - PrevTime);
        for (usize c = 0; c < N; ++c) {
            OutTangent[c] =
                S(1.f - Tension) * ((CurPoint[c] - PrevPoint[c]) + (NextPoint[c] - CurPoint[c]));
            OutTangent[c] = Divide(OutTangent[c], PrevToNextTimeDiff);
        }
    }

    void AutoSetTangents(float Tension, bool bStationaryEndpoints) noexcept {
        const int32 NumPoints = Num;
        const int32 LastPoint = NumPoints - 1;
        for (int32 PointIndex = 0; PointIndex < NumPoints; PointIndex++) {
            const int32 PrevIndex =
                (PointIndex == 0) ? (bIsLooped ? LastPoint : 0) : (PointIndex - 1);
            const int32 NextIndex =
                (PointIndex == LastPoint) ? (bIsLooped ? 0 : LastPoint) : (PointIndex + 1);
            Point<S, N>& ThisPoint = Points[PointIndex];
            const Point<S, N>& PrevPoint = Points[PrevIndex];
            const Point<S, N>& NextPoint = Points[NextIndex];
            const float PrevTime = (bIsLooped && PointIndex == 0)
                                       ? (ThisPoint.InVal - LoopKeyOffset)
                                       : PrevPoint.InVal;
            const float NextTime = (bIsLooped && PointIndex == LastPoint)
                                       ? (ThisPoint.InVal + LoopKeyOffset)
                                       : NextPoint.InVal;
            if (ThisPoint.InterpMode == CIM_CurveAuto ||
                ThisPoint.InterpMode == CIM_CurveAutoClamped) {
                if (bStationaryEndpoints &&
                    (PointIndex == 0 || (PointIndex == LastPoint && !bIsLooped))) {
                    for (usize c = 0; c < N; ++c) {
                        ThisPoint.ArriveTangent[c] = S(0);
                        ThisPoint.LeaveTangent[c] = S(0);
                    }
                } else if (PrevPoint.IsCurveKey()) {
                    S Tangent[N] = {};
                    ComputeCurveTangent(PrevTime, PrevPoint.OutVal, ThisPoint.InVal,
                                        ThisPoint.OutVal, NextTime, NextPoint.OutVal, Tension,
                                        ThisPoint.InterpMode == CIM_CurveAutoClamped, Tangent);
                    for (usize c = 0; c < N; ++c) {
                        ThisPoint.ArriveTangent[c] = Tangent[c];
                        ThisPoint.LeaveTangent[c] = Tangent[c];
                    }
                } else {
                    for (usize c = 0; c < N; ++c) {
                        ThisPoint.ArriveTangent[c] = PrevPoint.ArriveTangent[c];
                        ThisPoint.LeaveTangent[c] = PrevPoint.LeaveTangent[c];
                    }
                }
            } else if (ThisPoint.InterpMode == CIM_Linear) {
                const float leaveTime = FMathMax<float>(1.e-4f, NextTime - ThisPoint.InVal);
                const float arriveTime = FMathMax<float>(1.e-4f, ThisPoint.InVal - PrevTime);
                const bool prevIsCurve = PrevPoint.IsCurveKey();
                for (usize c = 0; c < N; ++c) {
                    ThisPoint.LeaveTangent[c] = NextPoint.OutVal[c] - ThisPoint.OutVal[c];
                    if (options.linearTangentIsSlope) {
                        ThisPoint.LeaveTangent[c] = Divide(ThisPoint.LeaveTangent[c], leaveTime);
                    }
                }

                for (usize c = 0; c < N; ++c) {
                    S arrive = ThisPoint.OutVal[c] - PrevPoint.OutVal[c];
                    if (options.linearTangentIsSlope) {
                        arrive = Divide(arrive, arriveTime);
                    }

                    ThisPoint.ArriveTangent[c] = prevIsCurve ? ThisPoint.LeaveTangent[c] : arrive;
                }
            } else if (ThisPoint.InterpMode == CIM_Constant) {
                for (usize c = 0; c < N; ++c) {
                    ThisPoint.ArriveTangent[c] = S(0);
                    ThisPoint.LeaveTangent[c] = S(0);
                }
            }
        }
    }
};

} // namespace ue58

namespace math_interp_curve_golden_tests_file {

namespace {

template <typename S>
bool SameBits(S a, S b) noexcept {
    if constexpr (IsSameV<S, float>) {
        return platform::BitCast<uint32>(a) == platform::BitCast<uint32>(b);
    } else {
        return platform::BitCast<uint64>(a) == platform::BitCast<uint64>(b);
    }
}

template <typename T>
using ScalarOf = typename CurveValueTraits<T>::Scalar;

template <typename T>
constexpr usize ComponentsOf = CurveValueTraits<T>::ComponentCount;

template <typename T>
T RandomValue(MathRandom& random) noexcept {
    ScalarOf<T> components[ComponentsOf<T>] = {};
    for (usize c = 0; c < ComponentsOf<T>; ++c) {
        components[c] = static_cast<ScalarOf<T>>(random.Range(-4.0, 4.0));
    }

    return CurveValueTraits<T>::Load(components);
}

template <typename T>
ue58::Point<ScalarOf<T>, ComponentsOf<T>> ToReference(const InterpCurvePoint<T>& point) noexcept {
    ue58::Point<ScalarOf<T>, ComponentsOf<T>> reference;
    reference.InVal = point.inValue;
    CurveValueTraits<T>::Store(point.outValue, reference.OutVal);
    CurveValueTraits<T>::Store(point.arriveTangent, reference.ArriveTangent);
    CurveValueTraits<T>::Store(point.leaveTangent, reference.LeaveTangent);
    reference.InterpMode = static_cast<int32>(point.mode);
    return reference;
}

template <typename T>
bool SameAsReference(const T& value, const ScalarOf<T> (&reference)[ComponentsOf<T>]) noexcept {
    ScalarOf<T> components[ComponentsOf<T>] = {};
    CurveValueTraits<T>::Store(value, components);
    for (usize c = 0; c < ComponentsOf<T>; ++c) {
        if (!SameBits(components[c], reference[c])) {
            return false;
        }
    }

    return true;
}

template <typename T>
bool PointMatchesReference(const InterpCurvePoint<T>& point,
                           const ue58::Point<ScalarOf<T>, ComponentsOf<T>>& reference) noexcept {
    return SameBits(point.inValue, reference.InVal) &&
           static_cast<int32>(point.mode) == reference.InterpMode &&
           SameAsReference(point.outValue, reference.OutVal) &&
           SameAsReference(point.arriveTangent, reference.ArriveTangent) &&
           SameAsReference(point.leaveTangent, reference.LeaveTangent);
}

// 무작위 커브 하나를 Rocks와 참조에 같은 순서로 넣는다. 기본은 입력값을 0.5 격자에서 골라 중복 키가
// 자주 생기고 간격이 1이 아닌 구간도 섞인다. unitSpacing이면 0..count−1의 서로 다른 정수 키를
// 회전한 순서로 넣는다(간격 1, 중복 없음). 루프·비루프를 고른다. withoutClamped면
// CurveAutoClamped를 CurveAuto로 바꾼다.
template <typename T>
void BuildRandomCurve(MathRandom& random, InterpCurve<T>& curve,
                      ue58::Curve<ScalarOf<T>, ComponentsOf<T>>& reference, bool unitSpacing,
                      bool withoutClamped) noexcept {
    const int32 count = 1 + static_cast<int32>(random.Next() % 8u);
    const int32 rotation = static_cast<int32>(random.Next() % 8u);
    for (int32 key = 0; key < count; ++key) {
        const float input =
            unitSpacing ? static_cast<float>((key + rotation) % count)
                        : static_cast<float>(static_cast<double>(random.Next() % 12u) * 0.5 - 2.0);
        InterpCurveMode mode = static_cast<InterpCurveMode>(random.Next() % 6u);
        if (withoutClamped && mode == InterpCurveMode::CurveAutoClamped) {
            mode = InterpCurveMode::CurveAuto;
        }

        const InterpCurvePoint<T> point(input, RandomValue<T>(random), RandomValue<T>(random),
                                        RandomValue<T>(random), mode);
        auto added = curve.TryAddPoint(point);
        const int32 referenceIndex = reference.AddPoint(ToReference(point));
        CHECK(added && added.Value() == static_cast<usize>(referenceIndex));
    }

    if (random.Next() % 2u == 0) {
        const float loopKey = curve.GetPoint(curve.PointCount() - 1).inValue +
                              (unitSpacing ? 1.0f : static_cast<float>(0.25 + random.Unit() * 2.0));
        REQUIRE(curve.TrySetLoopKey(loopKey));
        reference.SetLoopKey(loopKey);
    }
}

template <typename T>
void CheckCurveMatchesReference(const InterpCurve<T>& curve,
                                const ue58::Curve<ScalarOf<T>, ComponentsOf<T>>& reference,
                                MathRandom& random, int32& mismatches) noexcept {
    using S = ScalarOf<T>;
    REQUIRE(curve.PointCount() == static_cast<usize>(reference.Num));
    for (int32 index = 0; index < reference.Num; ++index) {
        if (!PointMatchesReference(curve.GetPoint(static_cast<usize>(index)),
                                   reference.Points[index])) {
            ++mismatches;
        }
    }

    const float first = reference.Points[0].InVal;
    const float last = reference.Points[reference.Num - 1].InVal +
                       (reference.bIsLooped ? reference.LoopKeyOffset : 0.0f);
    float inputs[64] = {};
    int32 inputCount = 0;
    for (int32 index = 0; index < reference.Num && inputCount < 32; ++index) {
        inputs[inputCount++] = reference.Points[index].InVal;
    }

    inputs[inputCount++] = last;
    while (inputCount < 64) {
        inputs[inputCount++] = static_cast<float>(
            random.Range(static_cast<double>(first) - 1.0, static_cast<double>(last) + 1.0));
    }

    for (float input : inputs) {
        S expected[ComponentsOf<T>] = {};
        reference.Evaluate(0, input, expected);
        if (!SameAsReference(curve.Eval(input), expected)) {
            ++mismatches;
        }

        reference.Evaluate(1, input, expected);
        if (!SameAsReference(curve.EvalDerivative(input), expected)) {
            ++mismatches;
        }

        reference.Evaluate(2, input, expected);
        if (!SameAsReference(curve.EvalSecondDerivative(input), expected)) {
            ++mismatches;
        }
    }
}

// 무작위 커브 cases개에서 키 삽입 순서·접선(사용자 접선과 AutoSetTangents)·값·1·2차 미분을 비트
// 단위로 대조한다. 나눗셈 방식은 형식의 성질(DividesByReciprocal)을 따른다.
template <typename T>
int32 CountReferenceMismatches(uint64 seed, int32 cases, ue58::Options options, bool unitSpacing,
                               bool withoutClamped) noexcept {
    SystemAllocator allocator;
    MathRandom random(seed);
    options.reciprocalDivision = CurveValueTraits<T>::DividesByReciprocal;
    int32 mismatches = 0;
    for (int32 sample = 0; sample < cases; ++sample) {
        InterpCurve<T> curve(allocator);
        ue58::Curve<ScalarOf<T>, ComponentsOf<T>> reference;
        reference.options = options;
        BuildRandomCurve(random, curve, reference, unitSpacing, withoutClamped);
        CheckCurveMatchesReference(curve, reference, random, mismatches);

        const float tension = static_cast<float>(random.Unit());
        const bool stationary = random.Next() % 2u == 0;
        curve.AutoSetTangents(tension, stationary);
        reference.AutoSetTangents(tension, stationary);
        CheckCurveMatchesReference(curve, reference, random, mismatches);
    }

    return mismatches;
}

// Rocks 의미(설계 문서 4절의 차이를 켠 참조)와의 비트 대조다.
template <typename T>
void CheckGoldenForType(uint64 seed) noexcept {
    ue58::Options rocksOptions;
    rocksOptions.clampTangents = CurveValueTraits<T>::ClampsAutoTangents;
    CHECK(CountReferenceMismatches<T>(seed, 300, rocksOptions, false, false) == 0);
}

// UE 그대로(차이를 끈 참조)와의 비트 대조다. 간격 1 정수 키에서는 Linear 접선 단위와 Lerp 끝점
// 차이가 값을 바꾸지 않는다. clampInUe는 UE가 그 형식에서 CurveAutoClamped를 의도대로
// clamp하는지다.
template <typename T>
void CheckUeLiteralForType(uint64 seed, bool clampInUe) noexcept {
    ue58::Options literal;
    literal.linearTangentIsSlope = false;
    literal.exactLerpEndpoints = false;
    literal.clampTangents = CurveValueTraits<T>::ClampsAutoTangents;
    const bool sameClamp = clampInUe == CurveValueTraits<T>::ClampsAutoTangents;
    CHECK(CountReferenceMismatches<T>(seed, 300, literal, true, !sameClamp) == 0);
}

} // namespace

TEST(InterpCurveGolden_MatchesUeFormulasBitExactly) {
    CheckGoldenForType<float>(0x1001);
    CheckGoldenForType<double>(0x1002);
    CheckGoldenForType<Vec2>(0x1003);
    CheckGoldenForType<Vec2d>(0x1004);
    CheckGoldenForType<Vec3>(0x1005);
    CheckGoldenForType<Vec3d>(0x1006);
    CheckGoldenForType<Vec4>(0x1007);
    CheckGoldenForType<Vec4d>(0x1008);
    CheckGoldenForType<LinearColor>(0x1009);
}

TEST(InterpCurveGolden_UeLiteralDiffersOnlyInDocumentedCases) {
    // UE float 커브는 clamp한다. FVector4·FLinearColor는 clamp를 무시하고(Rocks도 같다), UE의
    // FVector2f·FVector3f는 일반 템플릿이라 clamp하지 않으며 FVector2D·FVector는 결함 경로다 — 이
    // 형식들의 CurveAutoClamped 키는 대조에서 뺀다. double scalar 커브는 UE 형식이 없어 일반 템플릿
    // 기준(clamp 없음)이다.
    CheckUeLiteralForType<float>(0x2001, true);
    CheckUeLiteralForType<double>(0x2002, false);
    CheckUeLiteralForType<Vec2>(0x2003, false);
    CheckUeLiteralForType<Vec2d>(0x2004, false);
    CheckUeLiteralForType<Vec3>(0x2005, false);
    CheckUeLiteralForType<Vec4>(0x2006, false);
    CheckUeLiteralForType<Vec4d>(0x2007, false);
    CheckUeLiteralForType<LinearColor>(0x2008, false);

    // 간격이 1이 아니면 Linear 접선 단위와 Lerp 끝점 때문에 결과가 다르다.
    ue58::Options literal;
    literal.linearTangentIsSlope = false;
    literal.exactLerpEndpoints = false;
    CHECK(CountReferenceMismatches<float>(0x2009, 300, literal, false, false) > 0);

    // 반례 기록: 간격 2의 Linear → CurveAuto 이음매에서 UE 식은 1차 미분이 2배로 끊긴다.
    SystemAllocator allocator;
    InterpCurveFloat curve(allocator);
    ue58::Curve<float, 1> ue;
    ue.options = literal;
    const InterpCurveMode modes[] = {InterpCurveMode::Linear, InterpCurveMode::CurveAuto,
                                     InterpCurveMode::CurveAuto};
    const float values[] = {0.0f, 4.0f, 6.0f};
    for (usize index = 0; index < 3; ++index) {
        const InterpCurvePoint<float> point(2.0f * static_cast<float>(index), values[index], 0.0f,
                                            0.0f, modes[index]);
        REQUIRE(curve.TryAddPoint(point));
        ue.AddPoint(ToReference(point));
    }

    curve.AutoSetTangents(0.0f, false);
    ue.AutoSetTangents(0.0f, false);
    float ueLeft[1] = {};
    float ueRight[1] = {};
    ue.Evaluate(1, 1.999f, ueLeft);
    ue.Evaluate(1, 2.0f, ueRight);
    CHECK(ueLeft[0] == 2.0f && ueRight[0] == 4.0f);
    CHECK(curve.EvalDerivative(1.999f) == 2.0f && curve.EvalDerivative(2.0f) == 2.0f);

    // UE 저장 접선을 그대로 쓰면(다시 계산하지 않으면) 결과가 같다: Linear 키 접선은 평가에 쓰이지
    // 않고, 곡선 키 접선은 사용자 값 그대로다.
    InterpCurveFloat stored(allocator);
    for (int32 index = 0; index < 3; ++index) {
        REQUIRE(stored.TryAddPoint(ue.Points[index].InVal, ue.Points[index].OutVal[0]));
        stored.SetMode(static_cast<usize>(index),
                       static_cast<InterpCurveMode>(ue.Points[index].InterpMode));
        stored.SetTangents(static_cast<usize>(index), ue.Points[index].ArriveTangent[0],
                           ue.Points[index].LeaveTangent[0]);
    }

    for (int32 step = 0; step <= 50; ++step) {
        const float input = static_cast<float>(step) * 0.1f - 0.5f;
        float expected[1] = {};
        ue.Evaluate(0, input, expected);
        CHECK(stored.Eval(input) == expected[0]);
        ue.Evaluate(1, input, expected);
        CHECK(stored.EvalDerivative(input) == expected[0]);
    }
}

namespace {

// 독립 double Hermite: 멱급수 계수 c0 + c1 a + c2 a² + c3 a³(m0·m1은 구간 길이를 곱한 접선).
struct DoubleCubic {
    double c0;
    double c1;
    double c2;
    double c3;

    static DoubleCubic Make(double p0, double m0, double p1, double m1) noexcept {
        return DoubleCubic{p0, m0, -3.0 * p0 - 2.0 * m0 - m1 + 3.0 * p1,
                           2.0 * p0 + m0 + m1 - 2.0 * p1};
    }

    double Value(double a) const noexcept {
        return ((c3 * a + c2) * a + c1) * a + c0;
    }

    double Slope(double a) const noexcept {
        return (3.0 * c3 * a + 2.0 * c2) * a + c1;
    }

    double Curvature(double a) const noexcept {
        return 6.0 * c3 * a + 2.0 * c2;
    }

    double Scale() const noexcept {
        return math::Abs(c0) + math::Abs(c1) + math::Abs(c2) + math::Abs(c3);
    }
};

struct DoubleSample {
    double value;
    double slope;
    double curvature;
    double scale;
};

// 커브 정의(키 배열)만 읽어 독립 평가한다. 구간은 선형 탐색으로 찾는다. 계약대로 입력·구간 길이·
// 비율은 float로 구하고(UE와 같다) 나머지는 double로 계산한다.
template <typename T>
DoubleSample DoubleReference(const InterpCurve<T>& curve, float input, usize component) noexcept {
    using S = ScalarOf<T>;
    const usize count = curve.PointCount();
    auto componentOf = [component](const T& value) noexcept {
        S components[ComponentsOf<T>] = {};
        CurveValueTraits<T>::Store(value, components);
        return static_cast<double>(components[component]);
    };

    const float first = curve.GetPoint(0).inValue;
    const float lastKey = curve.GetPoint(count - 1).inValue;
    const float loopEnd = curve.IsLooped() ? lastKey + curve.LoopKeyOffset() : lastKey;
    if (input < first) {
        return DoubleSample{componentOf(curve.GetPoint(0).outValue), 0.0, 0.0, 1.0};
    }

    if (input >= loopEnd) {
        const usize held = curve.IsLooped() ? 0 : count - 1;
        return DoubleSample{componentOf(curve.GetPoint(held).outValue), 0.0, 0.0, 1.0};
    }

    usize start = 0;
    for (usize index = 0; index < count; ++index) {
        if (curve.GetPoint(index).inValue <= input) {
            start = index;
        }
    }

    const bool loopSegment = start == count - 1;
    const InterpCurvePoint<T>& from = curve.GetPoint(start);
    const InterpCurvePoint<T>& to = curve.GetPoint(loopSegment ? 0 : start + 1);
    const float lengthFloat = loopSegment ? curve.LoopKeyOffset() : to.inValue - from.inValue;
    const double length = static_cast<double>(lengthFloat);
    const double p0 = componentOf(from.outValue);
    const double p1 = componentOf(to.outValue);
    if (from.mode == InterpCurveMode::Constant) {
        return DoubleSample{p0, 0.0, 0.0, math::Abs(p0) + 1.0};
    }

    const double alpha = static_cast<double>((input - from.inValue) / lengthFloat);
    if (from.mode == InterpCurveMode::Linear) {
        return DoubleSample{p0 + (p1 - p0) * alpha, (p1 - p0) / length, 0.0,
                            math::Abs(p0) + math::Abs(p1) + 1.0};
    }

    const DoubleCubic cubic = DoubleCubic::Make(p0, componentOf(from.leaveTangent) * length, p1,
                                                componentOf(to.arriveTangent) * length);
    return DoubleSample{cubic.Value(alpha), cubic.Slope(alpha) / length,
                        cubic.Curvature(alpha) / (length * length), cubic.Scale() + 1.0};
}

// 상대 오차 relative는 성분 정밀도와 무관하게 float 수준이다: 비율과 Hermite 기저 계수가 float라
// (UE CubicInterp<T, float>) double 형식도 기저 반올림(약 6e-8)만큼 오차가 남는다.
template <typename T>
void CheckAgainstDoubleReference(uint64 seed, double relative) noexcept {
    using S = ScalarOf<T>;
    SystemAllocator allocator;
    MathRandom random(seed);
    int32 failures = 0;
    for (int32 sample = 0; sample < 200; ++sample) {
        InterpCurve<T> curve(allocator);
        const int32 count = 2 + static_cast<int32>(random.Next() % 6u);
        double input = random.Range(-3.0, 0.0);
        for (int32 key = 0; key < count; ++key) {
            const InterpCurveMode mode = static_cast<InterpCurveMode>(random.Next() % 6u);
            REQUIRE(curve.TryAddPoint(
                InterpCurvePoint<T>(static_cast<float>(input), RandomValue<T>(random),
                                    RandomValue<T>(random), RandomValue<T>(random), mode)));
            input += random.Range(0.25, 2.0);
        }

        if (random.Next() % 2u == 0) {
            REQUIRE(curve.TrySetLoopKey(static_cast<float>(input)));
        }

        curve.AutoSetTangents(static_cast<float>(random.Unit() * 0.5), random.Next() % 2u == 0);

        const float first = curve.GetPoint(0).inValue;
        const float lastKey = curve.GetPoint(curve.PointCount() - 1).inValue;
        const float end = curve.IsLooped() ? lastKey + curve.LoopKeyOffset() : lastKey;
        for (int32 probe = 0; probe < 100; ++probe) {
            const float at = static_cast<float>(
                random.Range(static_cast<double>(first) - 0.5, static_cast<double>(end) + 0.5));
            S value[ComponentsOf<T>] = {};
            S slope[ComponentsOf<T>] = {};
            S curvature[ComponentsOf<T>] = {};
            CurveValueTraits<T>::Store(curve.Eval(at), value);
            CurveValueTraits<T>::Store(curve.EvalDerivative(at), slope);
            CurveValueTraits<T>::Store(curve.EvalSecondDerivative(at), curvature);
            const bool inside = at >= first && at < end;
            for (usize c = 0; c < ComponentsOf<T>; ++c) {
                const DoubleSample expected = DoubleReference(curve, at, c);
                const double tolerance = relative * expected.scale;
                if (!Near(static_cast<double>(value[c]), expected.value, tolerance)) {
                    ++failures;
                }

                if (inside &&
                    !Near(static_cast<double>(slope[c]), expected.slope, 4.0 * tolerance)) {
                    ++failures;
                }

                if (inside && !Near(static_cast<double>(curvature[c]), expected.curvature,
                                    16.0 * tolerance)) {
                    ++failures;
                }
            }
        }
    }

    CHECK(failures == 0);
}

} // namespace

TEST(InterpCurveGolden_MatchesIndependentDoubleReference) {
    CheckAgainstDoubleReference<float>(0x3001, 2.0e-6);
    CheckAgainstDoubleReference<Vec3>(0x3002, 2.0e-6);
    CheckAgainstDoubleReference<LinearColor>(0x3003, 2.0e-6);
    CheckAgainstDoubleReference<double>(0x3004, 1.0e-6);
    CheckAgainstDoubleReference<Vec4d>(0x3005, 1.0e-6);
}

namespace {

// 독립 double 회전 수학: Hamilton 곱, 축-각 로그·지수, acos 기반 slerp.
struct RefQuat {
    double x;
    double y;
    double z;
    double w;
};

RefQuat Multiply(const RefQuat& a, const RefQuat& b) noexcept {
    return RefQuat{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                   a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                   a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                   a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

double RefDot(const RefQuat& a, const RefQuat& b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

RefQuat Negate(const RefQuat& q) noexcept {
    return RefQuat{-q.x, -q.y, -q.z, -q.w};
}

RefQuat Normalize(const RefQuat& q) noexcept {
    const double length = math::Sqrt(RefDot(q, q));
    return RefQuat{q.x / length, q.y / length, q.z / length, q.w / length};
}

// 회전 각 θ = 2·atan2(|v|, w), 축 n이면 로그는 n·θ/2다.
RefQuat RefLog(const RefQuat& q) noexcept {
    const double sine = math::Sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
    if (sine < 1.0e-300) {
        return RefQuat{0.0, 0.0, 0.0, 0.0};
    }

    const double half = math::Atan2(sine, q.w);
    return RefQuat{q.x / sine * half, q.y / sine * half, q.z / sine * half, 0.0};
}

RefQuat RefExp(const RefQuat& v) noexcept {
    const double half = math::Sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (half < 1.0e-300) {
        return RefQuat{0.0, 0.0, 0.0, 1.0};
    }

    const double s = math::Sin(half) / half;
    return RefQuat{v.x * s, v.y * s, v.z * s, math::Cos(half)};
}

RefQuat RefSlerp(const RefQuat& a, const RefQuat& b, double t) noexcept {
    const double cosine = math::Clamp(RefDot(a, b), -1.0, 1.0);
    const double angle = math::Acos(cosine);
    if (angle < 1.0e-9) {
        return Normalize(RefQuat{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                                 a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t});
    }

    const double wa = math::Sin((1.0 - t) * angle) / math::Sin(angle);
    const double wb = math::Sin(t * angle) / math::Sin(angle);
    return Normalize(RefQuat{a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb,
                             a.w * wa + b.w * wb});
}

RefQuat RefControl(const RefQuat& previous, const RefQuat& current, const RefQuat& next) noexcept {
    const RefQuat p = RefDot(current, previous) < 0.0 ? Negate(previous) : previous;
    const RefQuat n = RefDot(current, next) < 0.0 ? Negate(next) : next;
    const RefQuat inverse{-current.x, -current.y, -current.z, current.w};
    const RefQuat a = RefLog(Multiply(inverse, p));
    const RefQuat b = RefLog(Multiply(inverse, n));
    const RefQuat sum{-(a.x + b.x) / 4.0, -(a.y + b.y) / 4.0, -(a.z + b.z) / 4.0, 0.0};
    return Normalize(Multiply(current, RefExp(sum)));
}

template <typename S>
RefQuat ToRef(const Quaternion<S>& q) noexcept {
    return RefQuat{static_cast<double>(q.x), static_cast<double>(q.y), static_cast<double>(q.z),
                   static_cast<double>(q.w)};
}

// 두 회전 사이 각이다. acos는 1 근처에서 3e-8 rad 아래를 구분하지 못하므로 같은 반구로 맞춘 뒤
// 4·atan2(|a − b|, |a + b|)로 잰다.
template <typename S>
double RefAngle(const Quaternion<S>& value, const RefQuat& expected) noexcept {
    const RefQuat v = Normalize(ToRef(value));
    const RefQuat e = RefDot(v, expected) < 0.0 ? Negate(expected) : expected;
    const RefQuat difference{v.x - e.x, v.y - e.y, v.z - e.z, v.w - e.w};
    const RefQuat sum{v.x + e.x, v.y + e.y, v.z + e.z, v.w + e.w};
    return 4.0 *
           math::Atan2(math::Sqrt(RefDot(difference, difference)), math::Sqrt(RefDot(sum, sum)));
}

template <typename S>
void CheckQuaternionReference(uint64 seed, double tolerance) noexcept {
    SystemAllocator allocator;
    MathRandom random(seed);
    double worst = 0.0;
    for (int32 sample = 0; sample < 100; ++sample) {
        InterpCurve<Quaternion<S>> curve(allocator);
        const int32 count = 3 + static_cast<int32>(random.Next() % 4u);
        RefQuat keys[8] = {};
        for (int32 key = 0; key < count; ++key) {
            const Vec3d axis =
                Vec3d(random.Range(-1.0, 1.0), random.Range(-1.0, 1.0), random.Range(-1.0, 1.0))
                    .NormalizedOr(Vec3d::UnitZ());
            const Quatd rotation = Quatd::FromUnitAxisAngle(axis, random.Range(-3.0, 3.0));
            const Quaternion<S> stored(rotation);
            keys[key] = ToRef(stored);
            REQUIRE(curve.TryAddPoint(static_cast<float>(key), stored));
            curve.SetMode(static_cast<usize>(key), InterpCurveMode::CurveAuto);
        }

        curve.AutoSetTangents(0.0f, false);

        for (int32 segment = 0; segment + 1 < count; ++segment) {
            const RefQuat q0 = keys[segment];
            const RefQuat s0 =
                RefControl(keys[segment == 0 ? 0 : segment - 1], q0, keys[segment + 1]);
            const RefQuat q1 = keys[segment + 1];
            const RefQuat s1 =
                RefControl(q0, q1, keys[segment + 2 < count ? segment + 2 : segment + 1]);
            const bool flip = RefDot(q0, q1) < 0.0;
            const RefQuat end = flip ? Negate(q1) : q1;
            const RefQuat endControl = flip ? Negate(s1) : s1;
            for (int32 step = 0; step < 20; ++step) {
                // 커브의 비율은 float로 계산된다(키 간격 1). 참조도 같은 비율에서 평가한다.
                const float input = static_cast<float>(segment) + static_cast<float>(step) / 20.0f;
                const double t = static_cast<double>(input - static_cast<float>(segment));
                const RefQuat expected = RefSlerp(RefSlerp(q0, end, t), RefSlerp(s0, endControl, t),
                                                  2.0 * t * (1.0 - t));
                worst = math::Max(worst, RefAngle(curve.Eval(input), expected));
            }
        }
    }

    CHECK(worst < tolerance);
}

} // namespace

TEST(InterpCurveGolden_QuaternionMatchesDoubleSquadReference) {
    CheckQuaternionReference<double>(0x4001, 1.0e-10);
    CheckQuaternionReference<float>(0x4002, 5.0e-5);
}

namespace {

// 구간 위 최근접 거리의 double oracle이다. 400개 균등 표본에서 국소 최소(끝점 포함)마다 이웃 표본
// 사이를 황금분할로 좁혀 가장 작은 값을 고른다. 거리 제곱은 6차식이라 국소 최소가 많아야 셋이므로
// 표본 간격 안에 두 최소가 겹치지 않는 한 전역 최소를 놓치지 않는다. 구간 길이는 커브처럼 float다.
template <typename T>
double OracleSegmentDistance(const InterpCurve<T>& curve, usize segment,
                             const T& position) noexcept {
    using S = ScalarOf<T>;
    const InterpCurvePoint<T>& start = curve.GetPoint(segment);
    const bool loop = segment + 1 == curve.PointCount();
    const InterpCurvePoint<T>& end = curve.GetPoint(loop ? 0 : segment + 1);
    const double length =
        static_cast<double>(loop ? curve.LoopKeyOffset() : end.inValue - start.inValue);
    S p0[ComponentsOf<T>] = {};
    S p1[ComponentsOf<T>] = {};
    S m0[ComponentsOf<T>] = {};
    S m1[ComponentsOf<T>] = {};
    S target[ComponentsOf<T>] = {};
    CurveValueTraits<T>::Store(start.outValue, p0);
    CurveValueTraits<T>::Store(end.outValue, p1);
    CurveValueTraits<T>::Store(start.leaveTangent, m0);
    CurveValueTraits<T>::Store(end.arriveTangent, m1);
    CurveValueTraits<T>::Store(position, target);
    DoubleCubic cubics[ComponentsOf<T>] = {};
    for (usize c = 0; c < ComponentsOf<T>; ++c) {
        const double from = static_cast<double>(p0[c]);
        const double to = static_cast<double>(p1[c]);
        cubics[c] = start.mode == InterpCurveMode::Linear
                        ? DoubleCubic{from, to - from, 0.0, 0.0}
                        : DoubleCubic::Make(from, static_cast<double>(m0[c]) * length, to,
                                            static_cast<double>(m1[c]) * length);
    }

    auto distanceAt = [&](double t) noexcept {
        double sum = 0.0;
        for (usize c = 0; c < ComponentsOf<T>; ++c) {
            sum += math::Square(cubics[c].Value(t) - static_cast<double>(target[c]));
        }

        return sum;
    };

    constexpr int32 Samples = 400;
    double distances[Samples + 1] = {};
    for (int32 step = 0; step <= Samples; ++step) {
        distances[step] = distanceAt(static_cast<double>(step) / Samples);
    }

    double best = distances[0];
    for (int32 step = 0; step <= Samples; ++step) {
        const bool leftOk = step == 0 || distances[step] <= distances[step - 1];
        const bool rightOk = step == Samples || distances[step] <= distances[step + 1];
        if (!leftOk || !rightOk) {
            continue;
        }

        double low = static_cast<double>(step == 0 ? 0 : step - 1) / Samples;
        double high = static_cast<double>(step == Samples ? Samples : step + 1) / Samples;
        const double ratio = 0.6180339887498949;
        for (int32 iteration = 0; iteration < 80; ++iteration) {
            const double a = high - (high - low) * ratio;
            const double b = low + (high - low) * ratio;
            if (distanceAt(a) < distanceAt(b)) {
                high = b;
            } else {
                low = a;
            }
        }

        best = math::Min(best, math::Min(distances[step], distanceAt((low + high) * 0.5)));
    }

    return best;
}

template <typename T>
void CheckNearestAgainstOracle(uint64 seed, double relative) noexcept {
    using S = ScalarOf<T>;
    SystemAllocator allocator;
    MathRandom random(seed);
    int32 failures = 0;
    for (int32 sample = 0; sample < 60; ++sample) {
        InterpCurve<T> curve(allocator);
        const int32 count = 2 + static_cast<int32>(random.Next() % 5u);
        double input = 0.0;
        for (int32 key = 0; key < count; ++key) {
            // 최근접 탐색 oracle은 Linear·곡선 구간만 다룬다(Constant는 별도 시험).
            const InterpCurveMode mode =
                random.Next() % 4u == 0 ? InterpCurveMode::Linear : InterpCurveMode::CurveUser;
            REQUIRE(curve.TryAddPoint(
                InterpCurvePoint<T>(static_cast<float>(input), RandomValue<T>(random),
                                    RandomValue<T>(random), RandomValue<T>(random), mode)));
            input += random.Range(0.5, 2.0);
        }

        if (random.Next() % 2u == 0) {
            REQUIRE(curve.TrySetLoopKey(static_cast<float>(input)));
        }

        if (random.Next() % 2u == 0) {
            for (usize index = 0; index < curve.PointCount(); ++index) {
                if (curve.GetPoint(index).mode == InterpCurveMode::CurveUser) {
                    curve.SetMode(index, InterpCurveMode::CurveAuto);
                }
            }

            curve.AutoSetTangents(0.0f, random.Next() % 2u == 0);
        }

        for (int32 probe = 0; probe < 20; ++probe) {
            const T position = RandomValue<T>(random);
            auto found = curve.TryFindNearest(position);
            REQUIRE(found);
            double oracle = 1.0e300;
            for (usize segment = 0; segment < curve.SegmentCount(); ++segment) {
                oracle = math::Min(oracle, OracleSegmentDistance(curve, segment, position));
            }

            const double distance = static_cast<double>(found.Value().distanceSquared);
            const double tolerance = relative * (1.0 + oracle);
            // 찾은 거리는 oracle보다 크지 않고(놓친 근 없음), 표본보다 작을 수 없다(거짓 거리
            // 없음).
            if (distance > oracle + tolerance || distance < oracle - tolerance) {
                ++failures;
            }

            // 보고한 입력(float)에서 평가한 점의 거리가 보고한 거리와 맞는다. 입력이 float로
            // 반올림되므로 float 입력 해상도만큼 여유를 둔다.
            S evaluated[ComponentsOf<T>] = {};
            S target[ComponentsOf<T>] = {};
            CurveValueTraits<T>::Store(curve.Eval(found.Value().input), evaluated);
            CurveValueTraits<T>::Store(position, target);
            double check = 0.0;
            for (usize c = 0; c < ComponentsOf<T>; ++c) {
                check += math::Square(static_cast<double>(evaluated[c]) -
                                      static_cast<double>(target[c]));
            }

            if (!Near(check, distance, 1.0e-4 * (1.0 + distance))) {
                ++failures;
            }
        }
    }

    CHECK(failures == 0);
}

} // namespace

TEST(InterpCurveGolden_FindNearestMatchesDenseDoubleOracle) {
    CheckNearestAgainstOracle<Vec3d>(0x5001, 1.0e-10);
    CheckNearestAgainstOracle<Vec2d>(0x5002, 1.0e-10);
    CheckNearestAgainstOracle<double>(0x5003, 1.0e-10);
    CheckNearestAgainstOracle<Vec3>(0x5004, 1.0e-5);
    CheckNearestAgainstOracle<Vec4>(0x5005, 1.0e-5);
}
} // namespace math_interp_curve_golden_tests_file

} // namespace rocks::test
