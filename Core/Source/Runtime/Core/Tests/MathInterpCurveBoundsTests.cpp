#include <Core/Math/InterpCurve.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Memory.h>
#include <Platform/PlatformMath.h>
#include <TestSupport/MathTestSupport.h>
#include <TestSupport/Test.h>

// InterpCurve::TryCalcBounds를 UE 5.8 식을 그대로 옮긴 참조(ue58bounds)와 비트 단위로 대조한다
// (Docs/Design/CurveData.ko.md 12.2절 — CV9). 참조 함수 ↔ UE 5.8.4 원본(리뷰어 줄 단위 대조용):
//   ue58bounds::FindBounds    ↔ Core/Private/Math/UnrealMath.cpp 1512–1573
//   ue58bounds::IntervalBounds ↔ UnrealMath.cpp 1575–1662(Curve*FindIntervalBounds —
//                                성분마다 FindBounds 뒤 FMath::Min·Max)
//   ue58bounds::CalcBounds    ↔ Core/Public/Math/InterpCurve.h 746–772(CalcBounds)
//   ue58bounds::CubicInterp   ↔ Core/Public/Math/UnrealMathUtility.h 1219–1225(일반 템플릿, U = T)
//   ue58bounds::Min·Max       ↔ Core/Public/GenericPlatform/GenericPlatformMath.h 944–970
// 닫는 구간 수정(fixClosing — 끝 시간 = 마지막 키 + 루프 offset, 15절 22)을 켠 참조 = Rocks 의미라
// 모든 사례에서 같고, 끈 참조(UE 그대로)와는 루프 커브에서만 다르다. 독립 확인으로 촘촘한 Eval
// 표본이 범위 안인지도 본다. 무작위 사례는 고정 seed다.
namespace rocks::test {

namespace ue58bounds {

// GenericPlatformMath.h 966–970·944–949
template <typename T>
T Min(T A, T B) noexcept {
    return (A < B) ? A : B;
}

template <typename T>
T Max(T A, T B) noexcept {
    return (B < A) ? A : B;
}

// UnrealMathUtility.h IsNearlyZero(float, UE_SMALL_NUMBER)·(double, UE_DOUBLE_SMALL_NUMBER)
inline bool IsNearlyZero(float Value) noexcept {
    return platform::PlatformMath::Abs(Value) <= 1.e-8f;
}

inline bool IsNearlyZero(double Value) noexcept {
    return platform::PlatformMath::Abs(Value) <= 1.e-8;
}

// UnrealMathUtility.h 1219–1225(U = T)
template <typename T, typename U>
T CubicInterp(const T& P0, const T& T0, const T& P1, const T& T1, const U& A) noexcept {
    const U A2 = A * A;
    const U A3 = A2 * A;

    return T((((2 * A3) - (3 * A2) + 1) * P0) + ((A3 - (2 * A2) + A) * T0) + ((A3 - A2) * T1) +
             (((-2 * A3) + (3 * A2)) * P1));
}

// UnrealMath.cpp 1512–1573
template <typename T>
void FindBounds(T& OutMin, T& OutMax, T Start, T StartLeaveTan, float StartT, T End, T EndArriveTan,
                float EndT, bool bCurve) noexcept {
    OutMin = Min(Start, End);
    OutMax = Max(Start, End);

    if (bCurve) {
        T Diff = EndT - StartT;
        StartLeaveTan *= Diff;
        EndArriveTan *= Diff;

        const T a = 6.f * Start + 3.f * StartLeaveTan + 3.f * EndArriveTan - 6.f * End;
        const T b = -6.f * Start - 4.f * StartLeaveTan - 2.f * EndArriveTan + 6.f * End;
        const T c = StartLeaveTan;

        if (IsNearlyZero(a)) {
            if (!IsNearlyZero(b)) {
                const T x = -c / b;
                const T t = StartT + x * (EndT - StartT);
                if (t > StartT && t < EndT) {
                    const T Val = CubicInterp(Start, StartLeaveTan, End, EndArriveTan, x);
                    OutMin = Min(OutMin, Val);
                    OutMax = Max(OutMax, Val);
                }
            }
        } else {
            const T Discriminant = (b * b) - (4.f * a * c);
            if (Discriminant >= 0.f) {
                const T SqrtDisc = platform::PlatformMath::Sqrt(Discriminant);

                const T x0 = (-b + SqrtDisc) / (2.f * a);
                const T t0 = StartT + x0 * (EndT - StartT);
                if (t0 > StartT && t0 < EndT) {
                    const T Val = CubicInterp(Start, StartLeaveTan, End, EndArriveTan, x0);

                    OutMin = Min(OutMin, Val);
                    OutMax = Max(OutMax, Val);
                }

                const T x1 = (-b - SqrtDisc) / (2.f * a);
                const T t1 = StartT + x1 * (EndT - StartT);
                if (t1 > StartT && t1 < EndT) {
                    const T Val = CubicInterp(Start, StartLeaveTan, End, EndArriveTan, x1);

                    OutMin = Min(OutMin, Val);
                    OutMax = Max(OutMax, Val);
                }
            }
        }
    }
}

template <typename S, usize N>
struct Point {
    float InVal = 0.0f;
    S OutVal[N] = {};
    S ArriveTangent[N] = {};
    S LeaveTangent[N] = {};
    bool bCurve = false;
};

// UnrealMath.cpp 1575–1662: 성분마다 FindBounds 뒤 CurrentMin = Min(CurrentMin, OutMin),
// CurrentMax = Max(CurrentMax, OutMax). endInVal은 End.InVal(fixClosing이면 닫는 구간에서 바뀐다).
template <typename S, usize N>
void IntervalBounds(const Point<S, N>& Start, const Point<S, N>& End, float endInVal,
                    S (&CurrentMin)[N], S (&CurrentMax)[N]) noexcept {
    for (usize component = 0; component < N; ++component) {
        S OutMin;
        S OutMax;
        FindBounds(OutMin, OutMax, Start.OutVal[component], Start.LeaveTangent[component],
                   Start.InVal, End.OutVal[component], End.ArriveTangent[component], endInVal,
                   Start.bCurve);
        CurrentMin[component] = Min(CurrentMin[component], OutMin);
        CurrentMax[component] = Max(CurrentMax[component], OutMax);
    }
}

// InterpCurve.h 746–772(키 1개 이상). fixClosing이면 닫는 구간 끝 시간이 마지막 키 + offset이고 키
// 하나의 루프 커브도 닫는 구간을 본다.
template <typename S, usize N>
void CalcBounds(const Point<S, N>* Points, int32 NumPoints, bool bIsLooped, float LoopKeyOffset,
                bool fixClosing, S (&OutMin)[N], S (&OutMax)[N]) noexcept {
    for (usize component = 0; component < N; ++component) {
        OutMin[component] = Points[0].OutVal[component];
        OutMax[component] = Points[0].OutVal[component];
    }

    // fixClosing이면 키 하나의 루프 커브도 닫는 구간 하나를 본다(Rocks — Eval과 같은 구간 수).
    if (NumPoints == 1 && !(fixClosing && bIsLooped)) {
        return;
    }

    const int32 NumSegments = bIsLooped ? NumPoints : (NumPoints - 1);

    for (int32 Index = 0; Index < NumSegments; Index++) {
        const int32 NextIndex = (Index == NumPoints - 1) ? 0 : (Index + 1);
        const float endInVal = fixClosing && NextIndex == 0 ? Points[Index].InVal + LoopKeyOffset
                                                            : Points[NextIndex].InVal;
        IntervalBounds(Points[Index], Points[NextIndex], endInVal, OutMin, OutMax);
    }
}

} // namespace ue58bounds

namespace {

template <typename S>
bool SameBits(S a, S b) noexcept {
    if constexpr (sizeof(S) == 4) {
        return platform::BitCast<uint32>(a) == platform::BitCast<uint32>(b);
    } else {
        return platform::BitCast<uint64>(a) == platform::BitCast<uint64>(b);
    }
}

template <typename T>
using ScalarOf = typename CurveValueTraits<T>::Scalar;

template <typename T>
inline constexpr usize ComponentsOf = CurveValueTraits<T>::ComponentCount;

template <typename T>
T RandomValue(MathRandom& random, double range) noexcept {
    ScalarOf<T> components[ComponentsOf<T>];
    for (usize index = 0; index < ComponentsOf<T>; ++index) {
        const uint64 pick = random.Next() % 10;
        components[index] = pick == 0 ? static_cast<ScalarOf<T>>(0)
                                      : static_cast<ScalarOf<T>>(random.Range(-range, range));
    }

    return CurveValueTraits<T>::Load(components);
}

// 무작위 커브(키 1–8개, 같은 입력값 키, 모드 6종, 루프 있음·없음)를 Rocks와 참조에 같이 만든다.
template <typename T>
int32 RandomCurve(MathRandom& random, InterpCurve<T>& curve,
                  ue58bounds::Point<ScalarOf<T>, ComponentsOf<T>> (&reference)[8]) noexcept {
    const int32 count = 1 + static_cast<int32>(random.Next() % 8);
    float input = static_cast<float>(random.Range(-5.0, 5.0));
    for (int32 index = 0; index < count; ++index) {
        if (index > 0) {
            input += random.Next() % 6 == 0 ? 0.0f : static_cast<float>(random.Range(0.05, 3.0));
        }

        const T value = RandomValue<T>(random, 10.0);
        const T arrive = RandomValue<T>(random, 20.0);
        const T leave = random.Next() % 3 == 0 ? arrive : RandomValue<T>(random, 20.0);
        const auto mode = static_cast<InterpCurveMode>(random.Next() % 6);
        // 같은 입력값 키는 기존 키들 앞에 들어가므로 참조는 넣은 뒤의 배열에서 만든다(아래).
        CHECK(curve.TryAddPoint(InterpCurvePoint<T>(input, value, arrive, leave, mode)));
    }

    for (int32 index = 0; index < count; ++index) {
        const InterpCurvePoint<T>& added = curve.GetPoint(static_cast<usize>(index));
        auto& point = reference[index];
        point.InVal = added.inValue;
        CurveValueTraits<T>::Store(added.outValue, point.OutVal);
        CurveValueTraits<T>::Store(added.arriveTangent, point.ArriveTangent);
        CurveValueTraits<T>::Store(added.leaveTangent, point.LeaveTangent);
        point.bCurve = added.IsCurveKey();
    }

    if (random.Next() % 2 == 0) {
        CHECK(curve.TrySetLoopKey(input + static_cast<float>(random.Range(0.1, 3.0))));
    }

    return count;
}

template <typename T>
void CheckBoundsForType(uint64 seed) noexcept {
    using S = ScalarOf<T>;
    constexpr usize N = ComponentsOf<T>;
    SystemAllocator allocator;
    MathRandom random(seed);
    int32 mismatches = 0;
    int32 ueDifferences = 0;
    int32 loopedDifferences = 0;
    int32 outside = 0;
    for (int32 iteration = 0; iteration < 3000; ++iteration) {
        InterpCurve<T> curve(allocator);
        ue58bounds::Point<S, N> reference[8];
        const int32 count = RandomCurve<T>(random, curve, reference);
        auto bounds = curve.TryCalcBounds();
        REQUIRE(bounds);
        S minimum[N];
        S maximum[N];
        CurveValueTraits<T>::Store(bounds.Value().minimum, minimum);
        CurveValueTraits<T>::Store(bounds.Value().maximum, maximum);

        S fixedMin[N];
        S fixedMax[N];
        ue58bounds::CalcBounds(reference, count, curve.IsLooped(), curve.LoopKeyOffset(), true,
                               fixedMin, fixedMax);
        S ueMin[N];
        S ueMax[N];
        ue58bounds::CalcBounds(reference, count, curve.IsLooped(), curve.LoopKeyOffset(), false,
                               ueMin, ueMax);
        bool sameAsUe = true;
        for (usize component = 0; component < N; ++component) {
            mismatches += SameBits(minimum[component], fixedMin[component]) &&
                                  SameBits(maximum[component], fixedMax[component])
                              ? 0
                              : 1;
            sameAsUe = sameAsUe && SameBits(minimum[component], ueMin[component]) &&
                       SameBits(maximum[component], ueMax[component]);
        }

        if (!sameAsUe) {
            ++ueDifferences;
            loopedDifferences += curve.IsLooped() ? 1 : 0;
        }

        // 독립 확인: 키 범위(루프면 루프 끝까지)의 촘촘한 Eval 표본이 범위 안이다(반올림 여유).
        const float first = reference[0].InVal;
        const float last =
            reference[count - 1].InVal + (curve.IsLooped() ? curve.LoopKeyOffset() : 0.0f);
        for (int32 sample = 0; sample <= 400; ++sample) {
            const float input = first + (last - first) * (static_cast<float>(sample) / 400.0f);
            S value[N];
            CurveValueTraits<T>::Store(curve.Eval(input), value);
            for (usize component = 0; component < N; ++component) {
                const S width = maximum[component] - minimum[component];
                const S slack =
                    static_cast<S>(1.0e-4) *
                    (static_cast<S>(1) + width + platform::PlatformMath::Abs(value[component]));
                outside += value[component] >= minimum[component] - slack &&
                                   value[component] <= maximum[component] + slack
                               ? 0
                               : 1;
            }
        }
    }

    CHECK(mismatches == 0);
    CHECK(ueDifferences == loopedDifferences); // UE 그대로와는 루프 커브에서만 다르다
    CHECK(loopedDifferences > 0);
    CHECK(outside == 0);
}

} // namespace

// 지원 형식 일곱(UE 대응 float·Vector2d·Vector3d·LinearColor와 Rocks float 벡터·double)에서 무작위
// 커브의 TryCalcBounds가 닫는 구간 수정을 켠 UE 참조와 비트가 같다.
TEST(InterpCurveBounds_MatchUeFindBounds) {
    CheckBoundsForType<float>(0xB0D1);
    CheckBoundsForType<double>(0xB0D2);
    CheckBoundsForType<Vec2>(0xB0D3);
    CheckBoundsForType<Vec2d>(0xB0D4);
    CheckBoundsForType<Vec3>(0xB0D5);
    CheckBoundsForType<Vec3d>(0xB0D6);
    CheckBoundsForType<LinearColor>(0xB0D7);
}

// 손 계산 벡터:
// - 빈 커브는 NotFound, 키 하나는 그 값(루프면 자기 자신으로 가는 닫는 구간 — Linear면 그 값
// 그대로, 곡선이면
//   접선이 만드는 극값까지).
// - Linear 두 키 (0, 1)·(2, 5): [1, 5].
// - 곡선 두 키 (0, 0, 출발 접선 3)·(1, 0, 도착 접선 0): Hermite 3α − 6α² + 3α³의 극값 α = 1/3에서
//   3·(1/3) − 6/9 + 3/27 = 4/9(도함수 근 1은 구간 끝이라 넣지 않는다). 최소 0, 최대 4/9.
// - 루프 닫는 구간: 키 (0, 0, CurveUser, 접선 0)·(1, 0, CurveUser, 출발 접선 −3, 도착 0), 루프
// 끝 2.
//   닫는 구간(1 → 2, 첫 키로)의 Hermite는 −3α + 6α² − 3α³ (P0 = P1 = 0, T0 = −3, T1 = 0)이고 α =
//   1/3에서 −4/9다. UE는 끝 시간을 첫 키 0으로 넘겨 구간 길이 −1 — 접선이 +3이 되어 극값 +4/9를
//   찾지만 t가 구간 밖이라(StartT < t < EndT가 거짓) 넣지 않는다 — 최소 0.
TEST(InterpCurveBounds_HandVectors) {
    SystemAllocator allocator;
    InterpCurve<float> curve(allocator);
    CHECK(!curve.TryCalcBounds() && curve.TryCalcBounds().Error() == ErrorCode::NotFound);
    REQUIRE(curve.TryAddPoint(0.0f, 1.0f));
    REQUIRE(curve.TrySetLoopKey(3.0f));
    auto single = curve.TryCalcBounds();
    REQUIRE(single);
    CHECK(single.Value().minimum == 1.0f && single.Value().maximum == 1.0f);
    // 곡선 모드 키 하나의 루프: 닫는 구간 P0 = P1 = 1, T0 = 3, T1 = 0, 길이 3 — Hermite 1 + 9(α³ −
    // 2α² + α)의 극값 α = 1/3에서 1 + 9·4/27 = 7/3(UE는 키 하나면 1, 1이다).
    curve.SetMode(0, InterpCurveMode::CurveUser);
    curve.SetTangents(0, 0.0f, 3.0f);
    auto loopSingle = curve.TryCalcBounds();
    REQUIRE(loopSingle);
    CHECK(loopSingle.Value().minimum == 1.0f &&
          platform::PlatformMath::Abs(loopSingle.Value().maximum - 7.0f / 3.0f) < 1.0e-5f);
    CHECK(platform::PlatformMath::Abs(curve.Eval(1.0f) - 7.0f / 3.0f) < 1.0e-5f);
    curve.SetMode(0, InterpCurveMode::Linear);
    curve.SetTangents(0, 0.0f, 0.0f);
    curve.ClearLoopKey();
    REQUIRE(curve.TryAddPoint(2.0f, 5.0f));
    auto linear = curve.TryCalcBounds();
    REQUIRE(linear);
    CHECK(linear.Value().minimum == 1.0f && linear.Value().maximum == 5.0f);

    InterpCurve<double> bump(allocator);
    REQUIRE(bump.TryAddPoint(
        InterpCurvePoint<double>(0.0f, 0.0, 0.0, 3.0, InterpCurveMode::CurveUser)));
    REQUIRE(bump.TryAddPoint(
        InterpCurvePoint<double>(1.0f, 0.0, 0.0, 0.0, InterpCurveMode::CurveUser)));
    auto bumped = bump.TryCalcBounds();
    REQUIRE(bumped);
    CHECK(bumped.Value().minimum == 0.0);
    CHECK(platform::PlatformMath::Abs(bumped.Value().maximum - 4.0 / 9.0) < 1.0e-12);

    InterpCurve<double> loop(allocator);
    REQUIRE(loop.TryAddPoint(
        InterpCurvePoint<double>(0.0f, 0.0, 0.0, 0.0, InterpCurveMode::CurveUser)));
    REQUIRE(loop.TryAddPoint(
        InterpCurvePoint<double>(1.0f, 0.0, 0.0, -3.0, InterpCurveMode::CurveUser)));
    REQUIRE(loop.TrySetLoopKey(2.0f));
    auto looped = loop.TryCalcBounds();
    REQUIRE(looped);
    CHECK(platform::PlatformMath::Abs(looped.Value().minimum + 4.0 / 9.0) < 1.0e-12);
    CHECK(looped.Value().maximum == 0.0);
    CHECK(platform::PlatformMath::Abs(loop.Eval(1.0f + 1.0f / 3.0f) + 4.0 / 9.0) < 1.0e-6);

    ue58bounds::Point<double, 1> reference[2];
    reference[0].bCurve = reference[1].bCurve = true;
    reference[1].InVal = 1.0f;
    reference[1].LeaveTangent[0] = -3.0;
    double ueMin[1];
    double ueMax[1];
    ue58bounds::CalcBounds(reference, 2, true, 1.0f, false, ueMin, ueMax);
    CHECK(ueMin[0] == 0.0 && ueMax[0] == 0.0);

    // 값이 NaN이면 UE Min·Max 선택 규칙 그대로다(NaN 비교는 거짓 — 앞 값이 남거나 NaN이 들어온다).
    InterpCurve<float> nan(allocator);
    REQUIRE(nan.TryAddPoint(0.0f, platform::PlatformMath::QuietNaN<float>()));
    REQUIRE(nan.TryAddPoint(1.0f, 2.0f));
    auto withNaN = nan.TryCalcBounds();
    REQUIRE(withNaN);
    ue58bounds::Point<float, 1> nanReference[2];
    nanReference[0].OutVal[0] = platform::PlatformMath::QuietNaN<float>();
    nanReference[1].InVal = 1.0f;
    nanReference[1].OutVal[0] = 2.0f;
    float nanMin[1];
    float nanMax[1];
    ue58bounds::CalcBounds(nanReference, 2, false, 0.0f, true, nanMin, nanMax);
    CHECK(SameBits(withNaN.Value().minimum, nanMin[0]) &&
          SameBits(withNaN.Value().maximum, nanMax[0]));
}

} // namespace rocks::test
