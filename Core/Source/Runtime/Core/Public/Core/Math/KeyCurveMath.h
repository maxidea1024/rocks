#pragma once

#include <Base/TypeTraits.h>
#include <Base/Types.h>
#include <Core/Math/InterpCurveMath.h>
#include <Core/Math/MathUtility.h>

// 키 커브(TangentCurve·PlainCurve·PackedTangentCurve — Docs/Design/CurveData.ko.md)의 평가 핵심
// 식이다. 두 키 사이 보간(EvalCurveSegment), Bezier(BezierCurveValue), 가중
// 접선(WeightedCurveSegment)과 그 3차 방정식(SolveCurveCubic), 순환 외삽의 시간
// 재배치(CycleCurveTime), 자동 접선(ComputeCurveTangent), 그리고 UE FMath::SinCos(float)
// 다항식(CurveSinCos)이다.
//
// 기준은 UE 5.8 FRichCurve 평가(Engine/Private/Curves/CurveEvaluation.cpp·RealCurve.cpp,
// Public/Curves/CurveEvaluation.h)이고 같은 식·같은 연산 순서·같은 정밀도(float, UE가 double을 섞는
// 자리는 double)로 옮겼다. 빌드가 FMA 축약을 끄므로(-ffp-contract=off) Windows x64에서 UE
// /fp:precise 빌드와 비트가 같다. 다른 곳은 설계 문서 15절의 수정뿐이다:
// - Lerp는 math::Lerp라 비율 0·1에서 끝 값을 그대로 낸다(유한한 끝의 차가 넘칠 때만 UE와 다르다).
// - 순환 시간 재배치는 길이 0·비유한 입력에서 재배치하지 않고, 주기 몫이 2^30 이상이면(UE SSE
//   FloorToInt가 틀리는 범위) double로 재배치한다(Fatal 없음, D16).
// - 가중 접선 3차 방정식은 근이 2개일 때 채워지지 않은 세 번째 칸을 읽지 않는다.
// 가중 경로는 C 런타임의 atanf·acos·cos·pow·sqrt를 부른다. 이 함수들은 플랫폼마다 마지막 비트가
// 다를 수 있으므로 플랫폼 사이의 비트 동일성은 보장하지 않는다(UE와 같은 수준 — D17). CurveSinCos는
// C 런타임을 쓰지 않는 다항식이라 플랫폼과 무관하게 결정적이다.
//
// 모든 함수는 상태가 없고 할당하지 않으며 어느 스레드에서나 부를 수 있다. Fatal로 가는 경로가 없다.
namespace rocks::math {

// ---- 키 형식 ----

// 키 보간 방식의 번호다(UE ERichCurveInterpMode와 같은 번호). 키 형식의 interp 칸은 이 번호를
// 값으로 갖는 enum이어야 한다(Curves 모듈의 CurveInterp가 그렇다).
inline constexpr uint8 CurveInterpLinear = 0;
inline constexpr uint8 CurveInterpConstant = 1;
inline constexpr uint8 CurveInterpCubic = 2;

// 키 가중 방식의 번호다(UE ERichCurveTangentWeightMode와 같은 번호). 키 형식의 tangentWeight 칸은
// 이 번호를 값으로 갖는 enum이어야 한다(Curves 모듈의 CurveTangentWeight가 그렇다).
inline constexpr uint8 CurveWeightNone = 0;
inline constexpr uint8 CurveWeightArrive = 1;
inline constexpr uint8 CurveWeightLeave = 2;
inline constexpr uint8 CurveWeightBoth = 3;

// 평가 함수가 받는 키 형식이다. 칸 이름이 정해져 있다: float time·value·arriveTangent·
// arriveTangentWeight·leaveTangent·leaveTangentWeight, 그리고 위 번호를 쓰는 enum interp·
// tangentWeight. TangentCurveKey(Curves 모듈)와 압축 커브를 푼 키가 이 형식이다. 접선은 시간당 값
// 변화(기울기)이고 가중치는 시간·값 평면에서 키부터 Bezier 제어점까지의 거리다.
template <typename Key>
concept CurveSegmentKey = requires(const Key& key) {
    requires IsSameV<RemoveCvRefT<decltype(key.time)>, float>;
    requires IsSameV<RemoveCvRefT<decltype(key.value)>, float>;
    requires IsSameV<RemoveCvRefT<decltype(key.arriveTangent)>, float>;
    requires IsSameV<RemoveCvRefT<decltype(key.arriveTangentWeight)>, float>;
    requires IsSameV<RemoveCvRefT<decltype(key.leaveTangent)>, float>;
    requires IsSameV<RemoveCvRefT<decltype(key.leaveTangentWeight)>, float>;
    requires IsEnumV<RemoveCvRefT<decltype(key.interp)>>;
    requires IsEnumV<RemoveCvRefT<decltype(key.tangentWeight)>>;
};

namespace detail {

template <typename Enum>
constexpr uint8 CurveCode(Enum value) noexcept {
    return static_cast<uint8>(value);
}

// x64 cvttss2si(64비트)와 같은 float → int64 절단이다. 범위 밖·NaN은 C++에서 정의되지 않으므로
// x64가 내는 "정수 무효값"(int64 최솟값)을 명시적으로 낸다. UE SinCos의 (int64) 변환과 비트를
// 맞추려는 것이다.
inline int64 CurveTruncToInt64(float value) noexcept {
    constexpr float Limit = 9223372036854775808.0f; // 2^63
    if (value >= -Limit && value < Limit) {
        return static_cast<int64>(value);
    }

    return NumericLimits<int64>::Min();
}

} // namespace detail

// 구간(k1, k2)가 가중 접선을 쓰지 않는지다(UE UE::Curves::IsItNotWeighted, CE 193–197). 앞 키가
// None·Arrive이고 뒤 키가 None·Leave면 비가중이다. 키의 보간 방식과 무관한 구간 단위 판정이므로 뒤
// 키가 Linear여도 도착 가중이 있으면 Cubic 구간은 가중이다.
template <CurveSegmentKey Key>
constexpr bool IsCurveSegmentUnweighted(const Key& k1, const Key& k2) noexcept {
    const uint8 leave = detail::CurveCode(k1.tangentWeight);
    const uint8 arrive = detail::CurveCode(k2.tangentWeight);
    return (leave == CurveWeightNone || leave == CurveWeightArrive) &&
           (arrive == CurveWeightNone || arrive == CurveWeightLeave);
}

// ---- 기본 식 ----

// 3차 Bezier 값이다(UE UE::Curves::BezierInterp, CurveEvaluation.h 25–36 — De Casteljau). p0·p3은
// 끝 값, p1·p2는 제어점, alpha ∈ [0, 1]이다. 각 단계는 math::Lerp라 alpha가 0이면 p0, 1이면 p3을
// 그대로 낸다. alpha가 0·1이 아니면 UE A + α·(B − A)와 비트가 같다(유한한 끝의 차가 넘치는 경우
// 제외). 가중 경로는 3차 방정식 근을 float로 바꾼 값을 alpha로 넣으므로, UE와 비트가 갈리는 조건도
// 입력 비율이 아니라 이 alpha가 0·1인지다. NaN은 전파된다.
//
// 사용 예:
//   const float value = math::BezierCurveValue(0.0f, 1.0f, 2.0f, 3.0f, 0.5f);   // 1.5
constexpr float BezierCurveValue(float p0, float p1, float p2, float p3, float alpha) noexcept {
    const float p01 = Lerp(p0, p1, alpha);
    const float p12 = Lerp(p1, p2, alpha);
    const float p23 = Lerp(p2, p3, alpha);
    const float p012 = Lerp(p01, p12, alpha);
    const float p123 = Lerp(p12, p23, alpha);
    return Lerp(p012, p123, alpha);
}

// UE FMath::SinCos(float)(UnrealMathUtility.h 760–798)를 계수·연산 순서 그대로 옮긴
// 사인·코사인이다. 입력을 2π 배수로 줄여 [−π, π]로, 다시 ±π/2로 접은 뒤 11차(사인)·10차(코사인)
// minimax 다항식을 계산한다. 오차는 약 1e-7 이하(float 한두 ulp)다. C 런타임을 부르지 않으므로 같은
// 입력은 어느 플랫폼에서나 같은 비트를 낸다. 가중 접선 평가가 atanf 각도(|x| ≤ π/2)에 쓴다. NaN은
// NaN을 낸다. ±Inf와 아주 큰 |x|(약 5.8e19 이상)는 x64의 정수 변환 무효값을 따른 의미 없는 값(±Inf
// 등)이다(UE와 같다). 큰 각도는 float 2π로 줄이므로 |x|가 커질수록 오차가 커진다.
//
// 사용 예:
//   float sine, cosine;
//   math::CurveSinCos(0.5f, sine, cosine);
inline void CurveSinCos(float radians, float& sine, float& cosine) noexcept {
    // UE_INV_PI·UE_TWO_PI·UE_HALF_PI·UE_PI 리터럴 그대로다.
    constexpr float InvPi = 0.31830988618f;
    constexpr float TwoPiValue = 6.28318530717f;
    constexpr float HalfPiValue = 1.57079632679f;
    constexpr float PiValue = 3.1415926535897932f;

    float quotient = (InvPi * 0.5f) * radians;
    if (radians >= 0.0f) {
        quotient = static_cast<float>(detail::CurveTruncToInt64(quotient + 0.5f));
    } else {
        quotient = static_cast<float>(detail::CurveTruncToInt64(quotient - 0.5f));
    }

    float y = radians - TwoPiValue * quotient;
    float sign;
    if (y > HalfPiValue) {
        y = PiValue - y;
        sign = -1.0f;
    } else if (y < -HalfPiValue) {
        y = -PiValue - y;
        sign = -1.0f;
    } else {
        sign = +1.0f;
    }

    const float y2 = y * y;
    sine =
        (((((-2.3889859e-08f * y2 + 2.7525562e-06f) * y2 - 0.00019840874f) * y2 + 0.0083333310f) *
              y2 -
          0.16666667f) *
             y2 +
         1.0f) *
        y;
    const float p =
        ((((-2.6051615e-07f * y2 + 2.4760495e-05f) * y2 - 0.0013888378f) * y2 + 0.041666638f) * y2 -
         0.5f) *
            y2 +
        1.0f;
    cosine = sign * p;
}

// SolveCurveCubic의 결과다. roots[0..count)만 뜻이 있고 나머지 칸은 0이다.
struct CurveCubicRoots {
    double roots[3] = {0.0, 0.0, 0.0};
    uint32 count = 0;
};

// coefficients[0] + coefficients[1]·x + coefficients[2]·x² + coefficients[3]·x³ = 0의 실근을
// Cardano 닫힌 해로 구한다(UE UE::Curves::SolveCubic, CE 10–79 — Graphics Gems I). 연산 순서를 UE와
// 같게 두었다(수학적으로 같은 다른 순서는 비트가 다르다). 결과 근 수는 1–3이다:
// - 판별식 |D| ≤ 1e-8이면 |Q| ≤ 1e-8일 때 삼중근 하나, 아니면 단근과 중근 둘.
// - D < 0이면 서로 다른 세 실근(acos·cos), 그 밖은 실근 하나(pow 세제곱근).
// 최고차 계수가 0이면 1e-16으로 나누어 큰 근을 낸다(2차 이하 방정식을 따로 풀지 않는다 — UE와
// 같다). 계수에 NaN·Inf가 있으면 근도 NaN·Inf일 수 있다. 가중 접선 평가 외의 일반 다항식 근은
// PolynomialRoots.h를 쓴다(이 함수는 UE와 비트를 맞추는 것이 목적이다).
//
// 사용 예:
//   const double coefficients[4] = {-6.0, 11.0, -6.0, 1.0};   // (x−1)(x−2)(x−3)
//   const math::CurveCubicRoots solved = math::SolveCurveCubic(coefficients);   // count 3
inline CurveCubicRoots SolveCurveCubic(const double (&coefficients)[4]) noexcept {
    constexpr double SmallNumber = 1.e-8; // UE_DOUBLE_SMALL_NUMBER
    constexpr double PiValue = 3.141592653589793238462643383279502884197169399; // UE_DOUBLE_PI
    const auto cubeRoot = [](double x) noexcept -> double {
        return x > 0.0 ? platform::PlatformMath::Pow(x, 1.0 / 3.0)
                       : (x < 0.0 ? -platform::PlatformMath::Pow(-x, 1.0 / 3.0) : 0.0);
    };

    CurveCubicRoots result;
    const double denominator = coefficients[3] != 0.0 ? coefficients[3] : SmallNumber * SmallNumber;
    const double a = coefficients[2] / denominator;
    const double b = coefficients[1] / denominator;
    const double c = coefficients[0] / denominator;

    // x = y − a/3으로 2차 항을 없앤다: y³ + p·y + q = 0.
    const double squareOfA = a * a;
    const double p = 1.0 / 3 * (-1.0 / 3 * squareOfA + b);
    const double q = 1.0 / 2 * (2.0 / 27 * a * squareOfA - 1.0 / 3 * a * b + c);

    const double cubeOfP = p * p * p;
    const double discriminant = q * q + cubeOfP;
    if (Abs(discriminant) <= SmallNumber) {
        if (Abs(q) <= SmallNumber) {
            result.roots[0] = 0.0;
            result.count = 1;
        } else {
            const double u = cubeRoot(-q);
            result.roots[0] = 2 * u;
            result.roots[1] = -u;
            result.count = 2;
        }
    } else if (discriminant < 0) {
        const double phi = 1.0 / 3 * Acos(-q / Sqrt(-cubeOfP));
        const double t = 2 * Sqrt(-p);
        result.roots[0] = t * Cos(phi);
        result.roots[1] = -t * Cos(phi + PiValue / 3);
        result.roots[2] = -t * Cos(phi - PiValue / 3);
        result.count = 3;
    } else {
        const double squareRootOfD = Sqrt(discriminant);
        const double u = cubeRoot(squareRootOfD - q);
        const double v = -cubeRoot(squareRootOfD + q);
        result.roots[0] = u + v;
        result.count = 1;
    }

    const double substitute = 1.0 / 3 * a;
    for (uint32 index = 0; index < result.count; ++index) {
        result.roots[index] -= substitute;
    }

    return result;
}

// ---- 두 키 사이 ----

// 가중 접선 구간(k1, k2)의 Bezier 제어점이다(UE WeightedEvalForTwoKeys CE 97–143의 앞부분 —
// WeightedCurveSegment가 쓰는 값 그대로). 시간 축 제어점은 구간 [k1.time, k2.time]을 [0, 1]로
// 정규화한 값이고 값 축 제어점은 그대로다. 곡선은 시간 x(s) = Bezier(0, normalizedX1, normalizedX2,
// 1)(s), 값 y(s) = Bezier(k1.value, valueY1, valueY2, k2.value)(s)다(s ∈ [0, 1]).
// - normalizedX1·valueY1: k1 출발 제어점, normalizedX2·valueY2: k2 도착 제어점.
// k2.time > k1.time이어야 뜻이 있다(길이 0·NaN이면 정규화가 0 나눗셈이다 — 호출자가 먼저 거른다).
// 값 범위 계산(Curves 모듈 TangentCurve::TryGetValueRange)이 Evaluate와 같은 곡선을 보려고 쓴다.
struct CurveWeightedControls {
    float normalizedX1 = 0.0f;
    float normalizedX2 = 0.0f;
    float valueY1 = 0.0f;
    float valueY2 = 0.0f;
};

template <CurveSegmentKey Key>
CurveWeightedControls WeightedCurveControls(const Key& k1, const Key& k2) noexcept {
    const double time1 = k1.time;
    const double time2 = k2.time;
    const float oneThird = 1.0f / 3.0f;
    const float x = static_cast<float>(time2 - time1);

    float sine;
    float cosine;
    float angle = Atan(k1.leaveTangent);
    CurveSinCos(angle, sine, cosine);
    float leaveWeight;
    const uint8 leaveMode = detail::CurveCode(k1.tangentWeight);
    if (leaveMode == CurveWeightNone || leaveMode == CurveWeightArrive) {
        const float y = k1.leaveTangent * x;
        leaveWeight = Sqrt(x * x + y * y) * oneThird;
    } else {
        leaveWeight = k1.leaveTangentWeight;
    }

    const float key1TangentX =
        static_cast<float>(static_cast<double>(cosine * leaveWeight) + time1);
    const float key1TangentY = sine * leaveWeight + k1.value;

    angle = Atan(k2.arriveTangent);
    CurveSinCos(angle, sine, cosine);
    float arriveWeight;
    const uint8 arriveMode = detail::CurveCode(k2.tangentWeight);
    if (arriveMode == CurveWeightNone || arriveMode == CurveWeightLeave) {
        const float y = k2.arriveTangent * x;
        arriveWeight = Sqrt(x * x + y * y) * oneThird;
    } else {
        arriveWeight = k2.arriveTangentWeight;
    }

    const float key2TangentX =
        static_cast<float>(static_cast<double>(-cosine * arriveWeight) + time2);
    const float key2TangentY = -sine * arriveWeight + k2.value;

    // 시간 축을 [0, 1]로 정규화한다.
    const float rangeX = static_cast<float>(time2 - time1);
    const float dx1 = static_cast<float>(static_cast<double>(key1TangentX) - time1);
    const float dx2 = static_cast<float>(static_cast<double>(key2TangentX) - time1);
    const float normalizedX1 = dx1 / rangeX;
    const float normalizedX2 = dx2 / rangeX;
    return {normalizedX1, normalizedX2, key1TangentY, key2TangentY};
}

// 가중 접선 구간(k1, k2)의 time 값이다(UE UE::Curves::WeightedEvalForTwoKeys, CE 93–191). 시간 축과
// 값 축이 모두 Bezier인 곡선에서 "시간 Bezier x(s) = time"의 근 s를 Cardano로
// 구하고(SolveCurveCubic) 값 Bezier y(s)를 낸다.
// - k1의 출발 가중치는 k1.tangentWeight가 Leave·Both일 때 k1.leaveTangentWeight, 아니면 구간 길이로
//   만든 기본값 √(Δt² + (접선·Δt)²)/3이다. k2의 도착 가중치는 Arrive·Both일 때
//   k2.arriveTangentWeight다.
// - 키 시간은 double로 넓혀 시간 칸의 덧셈·뺄셈을 double로 계산한 뒤 float로 저장한다(UE 꼴 그대로
//   — float 두 값의 연산 한 번이라 float로 계산한 결과와 늘 같다, 설계 17.1절 K5). 제어점은
//   WeightedCurveControls다.
// - 근이 1개면 그 근, 여럿이면 [0, 1](허용오차 1e-8) 안의 근을 UE 순서로 고른다(앞에서부터 보며
//   고른 값이 음수이거나 더 큰 근이 나오면 바꾼다). 없으면 0이다. UE와 달리 근이 2개일 때 채워지지
//   않은 세 번째 칸을 읽지 않는다.
// k2.time > k1.time이어야 뜻이 있다. 아니면(길이 0·NaN) EvalCurveSegment처럼 k1.value를 낸다.
// 키의 보간 방식은 보지 않는다(호출자 EvalCurveSegment가 Cubic 구간에서만 부른다).
//
// 사용 예:
//   const float value = math::WeightedCurveSegment(key1, key2, 0.25f);
template <CurveSegmentKey Key>
float WeightedCurveSegment(const Key& k1, const Key& k2, float time) noexcept {
    const float difference = k2.time - k1.time;
    if (!(difference > 0.0f)) {
        return k1.value;
    }

    const float alpha = (time - k1.time) / difference;
    const CurveWeightedControls controls = WeightedCurveControls(k1, k2);
    const float normalizedX1 = controls.normalizedX1;
    const float normalizedX2 = controls.normalizedX2;

    // Bezier(0, x1, x2, 1)를 멱 기저로 바꾼다(UE BezierToPower, CE 81–91 — double).
    const double powerA = static_cast<double>(normalizedX1) - 0.0;
    const double powerB = static_cast<double>(normalizedX2) - static_cast<double>(normalizedX1);
    const double powerC = 1.0 - static_cast<double>(normalizedX2);
    const double powerD = powerB - powerA;
    double coefficients[4];
    coefficients[3] = powerC - powerB - powerD;
    coefficients[2] = 3.0 * powerD;
    coefficients[1] = 3.0 * powerA;
    coefficients[0] = 0.0;
    coefficients[0] = coefficients[0] - static_cast<double>(alpha);

    const CurveCubicRoots solved = SolveCurveCubic(coefficients);
    double parameter;
    if (solved.count == 1) {
        parameter = solved.roots[0];
    } else {
        constexpr double Tolerance = 1.e-8; // UE FMath::IsNearlyEqual(double) 기본값
        constexpr double Unset = NumericLimits<double>::Lowest();
        parameter = Unset;
        for (uint32 index = 0; index < solved.count; ++index) {
            const double root = solved.roots[index];
            const bool aboveZero = root > 0.0 || Abs(root - 0.0) <= Tolerance;
            const bool belowOne = root < 1.0 || Abs(root - 1.0) <= Tolerance;
            if (aboveZero && belowOne && (parameter < 0.0 || root > parameter)) {
                parameter = root;
            }
        }

        if (parameter == Unset) {
            parameter = 0.0;
        }
    }

    return BezierCurveValue(k1.value, controls.valueY1, controls.valueY2, k2.value,
                            static_cast<float>(parameter));
}

// 두 키 사이 time의 값이다(UE UE::Curves::EvalForTwoKeys, CE 205–250). 보간 방식은 앞 키의 것이고
// 접선은 앞 키의 출발 접선과 뒤 키의 도착 접선이다.
// - 길이(k2.time − k1.time)가 0 이하·NaN이면 k1.value.
// - Constant: time < k2.time이면 k1.value, 아니면 k2.value.
// - Linear: Lerp(k1.value, k2.value, α), α = (time − k1.time)/길이.
// - Cubic 비가중: 제어점 p1 = v1 + (출발 접선·길이)·⅓, p2 = v2 − (도착 접선·길이)·⅓의 Bezier.
// - Cubic 가중(IsCurveSegmentUnweighted가 거짓): WeightedCurveSegment.
// α가 [0, 1] 밖이어도 계산한다(외삽 — 호출자가 구간을 골라 부른다). NaN 시간은 Linear·Cubic에서
// NaN이지만 Constant 구간에서는 비교가 거짓이라 k2.value다(UE와 같다). 설계 5.6절의 NaN 규칙(키가
// 있으면 NaN)은 CV5 Evaluate가 구간을 고르기 전에 확인한다.
//
// 사용 예:
//   const float value = math::EvalCurveSegment(keys[index - 1], keys[index], time);
template <CurveSegmentKey Key>
float EvalCurveSegment(const Key& k1, const Key& k2, float time) noexcept {
    const float difference = k2.time - k1.time;
    if (!(difference > 0.0f)) {
        return k1.value;
    }

    const uint8 interp = detail::CurveCode(k1.interp);
    if (interp == CurveInterpConstant) {
        return time < k2.time ? k1.value : k2.value;
    }

    const float alpha = (time - k1.time) / difference;
    const float p0 = k1.value;
    const float p3 = k2.value;
    if (interp == CurveInterpLinear) {
        return Lerp(p0, p3, alpha);
    }

    if (IsCurveSegmentUnweighted(k1, k2)) {
        const float oneThird = 1.0f / 3.0f;
        const float p1 = p0 + (k1.leaveTangent * difference * oneThird);
        const float p2 = p3 - (k2.arriveTangent * difference * oneThird);
        return BezierCurveValue(p0, p1, p2, p3, alpha);
    }

    return WeightedCurveSegment(k1, k2, time);
}

// ---- 시간 재배치 ----

// CycleCurveTime의 결과다.
// - time: 키 범위 [첫 키 시간, 마지막 키 시간] 쪽으로 옮긴 시간(옮기지 않았으면 입력 그대로). 대개
//   범위 안이지만 int32 경로는 곱·합의 float 반올림으로 범위 끝을 벗어날 수 있다(|주기 몫|이 클수록
//   크게 — 약 2^22를 넘으면 한 주기 길이에 이를 수 있다, UE와 같은 식·같은 결과). 벗어난 시간은
//   Evaluate가 그 시간이 떨어진 쪽의 외삽 갈래로 받는다(UE와 같은 코드 경로). 그쪽 외삽이 Linear면
//   끝 키 값이 아니라 기울기로 이은 값이라 크게 달라질 수 있다 — UE와 비트가 같게 두고 자르지
//   않는다.
// - cycleCount: 옮긴 주기 수(0 이상의 정수 값). int32 경로는 UE CycleCount의 절댓값과 같다.
// - wide: 주기 몫이 2^30 이상이라 double로 재배치했는지. 이때 cycleCount는 double 정밀도의
//   근삿값이다(2^53 이상이면 홀짝이 뜻을 잃는다).
struct CurveCycle {
    float time = 0.0f;
    double cycleCount = 0.0;
    bool wide = false;
};

// 순환 외삽(Cycle·CycleWithOffset·Oscillate)의 시간 재배치다(UE FRealCurve::CycleTime,
// RealCurve.cpp 7–34, D16). time > maximum이면 뒤쪽, time < minimum이면 앞쪽 주기를 세어 키 범위
// 쪽으로 옮긴다.
// - 주기 몫은 UE 식의 float 몫((maximum − time)/길이 또는 (time − minimum)/길이)이다. |몫| <
// 2^30이면
//   UE와 같은 float 식(int32 주기)으로 옮긴다 — UE Windows x64의 SSE FloorToInt가 올바른 범위이고
//   그 안에서 UE와 비트가 같다. 옮긴 시간이 반대쪽 끝에 닿으면 UE처럼 바로잡는다(앞쪽에서 온
//   maximum은 minimum, 뒤쪽에서 온 minimum은 maximum).
// - |몫| ≥ 2^30이면 double 나머지(정확한 Fmod)로 옮긴다. 정확한 배수의 뒤쪽 시간은 maximum으로 가고
//   주기 수는 하나 줄인다(int32 경로의 정확한 배수 규칙과 같다). 끝 바로잡기는 하지 않는다(float
//   반올림으로 maximum이 된 앞쪽 시간은 maximum 그대로다 — 바꾸면 Cycle·Oscillate 결과가 뒤집힌다).
// - 길이(maximum − minimum)가 0 이하·비유한이거나 time이 비유한(±Inf·NaN)이면 옮기지 않는다(주기
//   0). UE는 0 나눗셈과 무효 정수 변환으로 의미 없는 주기 수를 낸다(15절 1).
// minimum ≤ time ≤ maximum이면 그대로다. 호출자는 앞·뒤 외삽 방식을 보고 이 함수를 부른 뒤
// CycleWithOffset이면 CurveCycleValueOffset, Oscillate면 IsOddCurveCycle을 쓴다.
//
// 사용 예:
//   const math::CurveCycle cycle = math::CycleCurveTime(0.0f, 2.0f, 5.0f);   // time 1, cycleCount
//   2
inline CurveCycle CycleCurveTime(float minimum, float maximum, float time) noexcept {
    // SSE FloorToInt(F + F − 0.5f)가 올바른 몫의 상한이다(UE와 비트가 같은 int32 경로의 범위).
    constexpr float SseFloorLimit = 1073741824.0f; // 2^30
    CurveCycle result;
    result.time = time;
    const float duration = maximum - minimum;
    const bool after = time > maximum;
    const bool before = time < minimum;
    if ((!after && !before) || !(duration > 0.0f) || !IsFinite(duration) || !IsFinite(time)) {
        return result;
    }

    const float ratio = after ? (maximum - time) / duration : (time - minimum) / duration;
    int32 cycles = 0;
    if (Abs(ratio) < SseFloorLimit && TryFloorToInt32(static_cast<double>(ratio), cycles)) {
        float moved = after ? time + duration * static_cast<float>(cycles)
                            : time - duration * static_cast<float>(cycles);
        if (moved == maximum && before) {
            moved = minimum;
        }

        if (moved == minimum && after) {
            moved = maximum;
        }

        result.time = moved;
        result.cycleCount = Abs(static_cast<double>(cycles));
        return result;
    }

    // 몫이 2^30 이상이다(아주 큰 시간·작은 길이). 정확한 Fmod로 나머지를 구하고 주기 수는 double로
    // 둔다. 나머지는 |r| < 길이이고, 0이 아니면 길이·2^-24보다 크다(시간 차 d ≥ 길이·2^30이라 d의
    // double 간격이 길이의 float 간격 이상이고, 나머지는 그 간격의 배수다). 그래서 음수 나머지에
    // 길이를 더해도 길이로 반올림되지 않는다.
    const double offset = static_cast<double>(time) - static_cast<double>(minimum);
    const double length = static_cast<double>(duration);
    double remainder = Fmod(offset, length);
    double quotient = Floor((offset - remainder) / length + 0.5);
    if (remainder < 0.0) {
        remainder += length;
        quotient -= 1.0;
    }

    if (after && remainder == 0.0) {
        result.time = maximum;
        result.cycleCount = quotient - 1.0;
    } else {
        result.time = static_cast<float>(static_cast<double>(minimum) + remainder);
        result.cycleCount = Abs(quotient);
    }

    result.wide = true;
    return result;
}

// CycleWithOffset 외삽의 값 오프셋이다. valueDelta는 뒤쪽이면 (마지막 키 값 − 첫 키 값), 앞쪽이면
// (첫 키 값 − 마지막 키 값)이다(UE RemapTimeValue, RC 1104–1161). int32 경로는 UE와 같은 float 곱
// valueDelta·(float)주기 수, double 경로는 double 곱을 float로 반올림한 값이다.
inline float CurveCycleValueOffset(const CurveCycle& cycle, float valueDelta) noexcept {
    if (cycle.wide) {
        return static_cast<float>(static_cast<double>(valueDelta) * cycle.cycleCount);
    }

    return valueDelta * static_cast<float>(cycle.cycleCount);
}

// Oscillate 외삽에서 이번 주기가 뒤집힌 주기(홀수)인지다. 뒤집힌 주기면 호출자가
// time = minimum + (maximum − time)으로 바꾼다(UE RemapTimeValue와 같다).
inline bool IsOddCurveCycle(const CurveCycle& cycle) noexcept {
    return Fmod(cycle.cycleCount, 2.0) == 1.0;
}

// ---- 자동 접선 ----

// 자동(Auto) Cubic 키의 접선이다(UE ComputeCurveTangent의 clamp 없는 경로, InterpCurvePoint.h
// 240–301 — FRichCurve::AutoSetTangents가 쓰는 경로):
//   (1 − tension)·((current − previous) + (next − current)) /
//   CurveTangentTimeDifference(previousTime, nextTime)
// 괄호 안을 next − previous로 줄이지 않는다(비트가 다르다). 시간 차 하한은 1e-4다(키 간격이 그보다
// 작으면 접선이 작아진다). tension 0은 Catmull-Rom 꼴, 1은 접선 0이다. NaN은 전파된다.
//
// 사용 예:
//   const float tangent = math::ComputeCurveTangent(0.0f, 0.0f, 1.0f, 2.0f, 2.0f, 0.0f);   // 1
inline float ComputeCurveTangent(float previousTime, float previousValue, float currentValue,
                                 float nextTime, float nextValue, float tension) noexcept {
    const float tangent =
        (1.0f - tension) * ((currentValue - previousValue) + (nextValue - currentValue));
    return tangent / CurveTangentTimeDifference(previousTime, nextTime);
}

} // namespace rocks::math
