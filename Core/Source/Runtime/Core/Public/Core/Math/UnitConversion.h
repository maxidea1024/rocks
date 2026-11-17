#pragma once

#include <Core/Math/Vector3.h>

namespace rocks::math {

// 물리 단위 경계 변환이다. Rocks의 공간 단위는 UE 5.8과 같은 cm·kg·초이고(MathUtility.h 머리
// 주석), 외부 계층(물리 엔진·외부 파일 형식 등)이 m·kg·초(SI)를 쓸 때 그 경계에서 이 함수로 바꾼다.
// 변환 계수를 호출 지점에 숫자로 흩어 쓰지 않는다.
//
// 계수는 물리량의 길이 차원에 따라 다르다. 일괄 100배가 아니다:
//   길이·속도·가속도·힘·선형 충격량(길이 1차)   cm 계열 = m 계열 × 100
//   넓이·토크·관성 모멘트(길이 2차)              cm 계열 = m 계열 × 10000
//   질량(kg)·시간(초)·각도·각속도(rad/s)        바꾸지 않는다(길이 차원이 없다)
// UE 근거(5.8.4): Core/Public/Math/UnrealMathUtility.h:177-183(UE_CM_TO_M·UE_M_TO_CM·
// UE_CM2_TO_M2·UE_M2_TO_CM2), Core/Private/Math/UnitConversion.cpp:723(cm → m 0.01)·:752-753
// (cm/s·m/s)·:775(cm/s² → m/s² 0.01)·:818(kg·cm/s² → N 0.01)·:828(kg·cm²/s² → N·m 0.0001),
// Engine/Classes/Components/PrimitiveComponent.h:2863(관성 텐서 kg·cm²). 선형 충격량 kg·cm/s는
// UE EUnit에 없고(UE 충격량 단위는 N·s — UnitConversion.cpp:590) 차원으로 정한 계수다. UE 물리
// API(AddForce·AddImpulse·AddTorque)가 실제로 어느 단위를 받는지는 물리 어댑터 설계에서 검증한다.
//
// 정확도: m 계열 → cm 계열은 정확한 정수 계수(100·10000)를 한 번 곱하고, cm 계열 → m 계열은 같은
// 계수로 한 번 나눈다. 따라서 결과는 정확한 값을 T로 한 번 반올림한 값이다(예: 980 cm/s² → 정확히
// 9.8에 가장 가까운 double). UE 매크로 UE_CM_TO_M은 float 0.01f(= 0.0099999997…)라 double 값에
// 곱하면 상대 오차 약 2.2e-8이 생긴다 — Rocks는 이 차이를 따르지 않는다(정확도 정책).
// 입력이 NaN·Inf이면 결과도 그대로 비유한이다. 아주 큰 유한값을 cm 계열로 바꾸면 T 범위를 넘어
// ±Inf가 될 수 있다(검사하지 않는다 — 호출자가 IsFinite로 확인한다).
//
// 값 타입: float, double, Vector3<float>, Vector3<double>(성분별로 같은 계산). 위치·속도 같은
// 벡터와 축별 관성(대각 텐서)·토크 벡터에 그대로 쓴다. 극성·축성 벡터의 좌우수 변환은 하지
// 않는다(단위만 바꾼다 — 좌표계 변환은 각 어댑터 경계의 몫이다).
//
// 사용 예(물리 엔진이 m 단위일 때):
//   const Vec3d gravityMetersPerSecondSquared =
//       math::CentimetersPerSecondSquaredToMetersPerSecondSquared(Vec3d(0.0, 0.0, -980.0));
//   // (0, 0, -9.8)
//   const double inertiaCm = math::KilogramSquareMetersToKilogramSquareCentimeters(0.004); // 40

// 1 m = 100 cm. 길이 1차 물리량의 계수다(UE UE_M_TO_CM).
template <typename T>
    requires IsFloatingPointV<T>
inline constexpr T CentimetersPerMeter = static_cast<T>(100);

// 1 m² = 10000 cm². 길이 2차 물리량(넓이·토크·관성)의 계수다(UE UE_M2_TO_CM2).
template <typename T>
    requires IsFloatingPointV<T>
inline constexpr T SquareCentimetersPerSquareMeter = static_cast<T>(10000);

namespace detail {

// 단위 변환 함수가 받는 값 타입과 그 scalar 형식이다.
template <typename V>
struct UnitValueTraits {
    static constexpr bool Supported = false;
};

template <>
struct UnitValueTraits<float> {
    static constexpr bool Supported = true;
    using Scalar = float;
};

template <>
struct UnitValueTraits<double> {
    static constexpr bool Supported = true;
    using Scalar = double;
};

template <typename T>
struct UnitValueTraits<Vector3<T>> {
    static constexpr bool Supported = IsFloatingPointV<T>;
    using Scalar = T;
};

template <typename V>
inline constexpr bool IsUnitValueV = UnitValueTraits<V>::Supported;

template <typename V>
using UnitScalar = typename UnitValueTraits<V>::Scalar;

// cm 계열 → m 계열: 길이 1차.
template <typename V>
constexpr V DivideByLinearFactor(const V& value) noexcept {
    return value / CentimetersPerMeter<UnitScalar<V>>;
}

// m 계열 → cm 계열: 길이 1차.
template <typename V>
constexpr V MultiplyByLinearFactor(const V& value) noexcept {
    return value * CentimetersPerMeter<UnitScalar<V>>;
}

// cm 계열 → m 계열: 길이 2차.
template <typename V>
constexpr V DivideBySquareFactor(const V& value) noexcept {
    return value / SquareCentimetersPerSquareMeter<UnitScalar<V>>;
}

// m 계열 → cm 계열: 길이 2차.
template <typename V>
constexpr V MultiplyBySquareFactor(const V& value) noexcept {
    return value * SquareCentimetersPerSquareMeter<UnitScalar<V>>;
}

} // namespace detail

// ---- 길이(위치·거리) ----

// cm → m. 예: 250 cm → 2.5 m.
template <typename V>
    requires detail::IsUnitValueV<V>
constexpr V CentimetersToMeters(const V& centimeters) noexcept {
    return detail::DivideByLinearFactor(centimeters);
}

// m → cm. 예: 2.5 m → 250 cm.
template <typename V>
    requires detail::IsUnitValueV<V>
constexpr V MetersToCentimeters(const V& meters) noexcept {
    return detail::MultiplyByLinearFactor(meters);
}

// ---- 넓이 ----

// cm² → m². 예: 10000 cm² → 1 m².
template <typename V>
    requires detail::IsUnitValueV<V>
constexpr V SquareCentimetersToSquareMeters(const V& squareCentimeters) noexcept {
    return detail::DivideBySquareFactor(squareCentimeters);
}

// m² → cm².
template <typename V>
    requires detail::IsUnitValueV<V>
constexpr V SquareMetersToSquareCentimeters(const V& squareMeters) noexcept {
    return detail::MultiplyBySquareFactor(squareMeters);
}

// ---- 속도 ----

// cm/s → m/s. 예: UE CharacterMovement 기본 걷기 속도 600 cm/s(CharacterMovementComponent.cpp:694)
// → 6 m/s.
template <typename V>
    requires detail::IsUnitValueV<V>
constexpr V CentimetersPerSecondToMetersPerSecond(const V& centimetersPerSecond) noexcept {
    return detail::DivideByLinearFactor(centimetersPerSecond);
}

// m/s → cm/s.
template <typename V>
    requires detail::IsUnitValueV<V>
constexpr V MetersPerSecondToCentimetersPerSecond(const V& metersPerSecond) noexcept {
    return detail::MultiplyByLinearFactor(metersPerSecond);
}

// ---- 가속도(중력 포함) ----

// cm/s² → m/s². 예: UE 기본 중력 -980 cm/s²(PhysicsSettingsCore.cpp:29) → -9.8 m/s², Chaos 기본
// -980.665 cm/s²(Chaos/PerParticleGravity.h:17) → -9.80665 m/s².
template <typename V>
    requires detail::IsUnitValueV<V>
constexpr V
CentimetersPerSecondSquaredToMetersPerSecondSquared(const V& centimetersPerSecondSquared) noexcept {
    return detail::DivideByLinearFactor(centimetersPerSecondSquared);
}

// m/s² → cm/s².
template <typename V>
    requires detail::IsUnitValueV<V>
constexpr V
MetersPerSecondSquaredToCentimetersPerSecondSquared(const V& metersPerSecondSquared) noexcept {
    return detail::MultiplyByLinearFactor(metersPerSecondSquared);
}

// ---- 힘 ----

// kg·cm/s² → N(kg·m/s²). 예: 질량 1 kg에 걸리는 중력 980 kg·cm/s² → 9.8 N.
template <typename V>
    requires detail::IsUnitValueV<V>
constexpr V KilogramCentimetersPerSecondSquaredToNewtons(
    const V& kilogramCentimetersPerSecondSquared) noexcept {
    return detail::DivideByLinearFactor(kilogramCentimetersPerSecondSquared);
}

// N → kg·cm/s².
template <typename V>
    requires detail::IsUnitValueV<V>
constexpr V NewtonsToKilogramCentimetersPerSecondSquared(const V& newtons) noexcept {
    return detail::MultiplyByLinearFactor(newtons);
}

// ---- 선형 충격량(운동량 변화) ----

// kg·cm/s → N·s(kg·m/s).
template <typename V>
    requires detail::IsUnitValueV<V>
constexpr V
KilogramCentimetersPerSecondToNewtonSeconds(const V& kilogramCentimetersPerSecond) noexcept {
    return detail::DivideByLinearFactor(kilogramCentimetersPerSecond);
}

// N·s → kg·cm/s.
template <typename V>
    requires detail::IsUnitValueV<V>
constexpr V NewtonSecondsToKilogramCentimetersPerSecond(const V& newtonSeconds) noexcept {
    return detail::MultiplyByLinearFactor(newtonSeconds);
}

// ---- 토크 ----

// kg·cm²/s² → N·m(kg·m²/s²). 예: 10000 kg·cm²/s² → 1 N·m.
template <typename V>
    requires detail::IsUnitValueV<V>
constexpr V KilogramCentimetersSquaredPerSecondSquaredToNewtonMeters(
    const V& kilogramCentimetersSquaredPerSecondSquared) noexcept {
    return detail::DivideBySquareFactor(kilogramCentimetersSquaredPerSecondSquared);
}

// N·m → kg·cm²/s².
template <typename V>
    requires detail::IsUnitValueV<V>
constexpr V
NewtonMetersToKilogramCentimetersSquaredPerSecondSquared(const V& newtonMeters) noexcept {
    return detail::MultiplyBySquareFactor(newtonMeters);
}

// ---- 관성 모멘트 ----

// kg·cm² → kg·m². 예: 질량 1 kg·반지름 10 cm 속찬 구의 관성 40 kg·cm² → 0.004 kg·m².
template <typename V>
    requires detail::IsUnitValueV<V>
constexpr V
KilogramSquareCentimetersToKilogramSquareMeters(const V& kilogramSquareCentimeters) noexcept {
    return detail::DivideBySquareFactor(kilogramSquareCentimeters);
}

// kg·m² → kg·cm².
template <typename V>
    requires detail::IsUnitValueV<V>
constexpr V
KilogramSquareMetersToKilogramSquareCentimeters(const V& kilogramSquareMeters) noexcept {
    return detail::MultiplyBySquareFactor(kilogramSquareMeters);
}

} // namespace rocks::math
