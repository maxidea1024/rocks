#pragma once

#include <Core/Math/Quaternion.h>
#include <Core/Math/UnitConversion.h>
#include <Core/Math/Vector3.h>

// Jolt.h가 Jolt의 모든 헤더보다 먼저 와야 한다(Core.h의 설정).
// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
// clang-format on

// Rocks ↔ Jolt 값 변환이다(Docs/Design/PhysicsJoltAdapter.ko.md 2·3·6·7절, PhysicsModule 7.1.2절).
// Rocks는 UE와 같은 왼손 +X 전방·+Y 오른쪽·+Z 위·cm·kg·초이고, Jolt는 오른손 Y-up·m다. 방향은
// 행렬식 +1인 축 순환 A(j = (r.y, r.z, r.x))로만 바꾼다 — 극성·축성 벡터·quaternion 모두 같은 A이고
// 부호 보정이 없다(2절). 크기는 Core UnitConversion 함수로만 바꾸며 물리량마다 길이 차원이 다르므로
// 함수를 물리량별로 나눈다(7절 — 같은 Vec3d라도 위치·속도·힘·토크의 계수가 다르다). 크기 변환은
// double에서 한 뒤 Jolt float로 한 번 반올림한다.
//
// 사용 예:
//   const JPH::RVec3 position = ToJoltPosition(component.WorldLocation(), regionOrigin);
//   const JPH::Vec3 velocity = ToJoltLinearVelocity(Vec3d(0.0, 0.0, -980.0));   // (0, -9.8, 0) m/s
//   const Vec3d back = FromJoltPosition(body.GetPosition(), regionOrigin);
//
// 이 헤더는 Physics private이다(Jolt 타입이 공개 헤더로 나가지 않는다 — 10.3절).
namespace rocks::physics {

// ---- 축 사상 A(크기 변환 없음) ----

// Rocks 성분을 Jolt 축으로 옮긴다: Forward(+X) → +Z, Right(+Y) → +X, Up(+Z) → +Y.
inline JPH::Vec3 AxisToJolt(const Vec3d& rocks) noexcept {
    return JPH::Vec3(static_cast<float>(rocks.y), static_cast<float>(rocks.z),
                     static_cast<float>(rocks.x));
}

// AxisToJolt의 역이다.
inline Vec3d AxisFromJolt(JPH::Vec3Arg jolt) noexcept {
    return Vec3d(static_cast<double>(jolt.GetZ()), static_cast<double>(jolt.GetX()),
                 static_cast<double>(jolt.GetY()));
}

// ---- 위치·길이(cm ↔ m) ----

// 월드 위치(cm)를 지역 원점(cm) 기준 Jolt 위치(m)로 바꾼다. 원점은 double에서 먼저 뺀다(6절 —
// 큰 월드에서 float 반올림 전에 크기를 줄인다).
inline JPH::RVec3 ToJoltPosition(const Vec3d& worldCentimeters,
                                 const Vec3d& originCentimeters) noexcept {
    return AxisToJolt(math::CentimetersToMeters(worldCentimeters - originCentimeters));
}

// ToJoltPosition의 역이다(Jolt 위치 m → 월드 위치 cm).
inline Vec3d FromJoltPosition(JPH::RVec3Arg meters, const Vec3d& originCentimeters) noexcept {
    return math::MetersToCentimeters(AxisFromJolt(meters)) + originCentimeters;
}

// 원점이 없는 변위·상대 위치(cm → m — 질량 중심 오프셋·compound 자식 위치 등)다.
inline JPH::Vec3 ToJoltDisplacement(const Vec3d& centimeters) noexcept {
    return AxisToJolt(math::CentimetersToMeters(centimeters));
}

inline Vec3d FromJoltDisplacement(JPH::Vec3Arg meters) noexcept {
    return math::MetersToCentimeters(AxisFromJolt(meters));
}

// box 반폭(축별 양수 크기, cm → m)이다. 성분을 재배치만 한다(부호 없음).
inline JPH::Vec3 ToJoltHalfExtents(const Vec3d& halfExtentCentimeters) noexcept {
    return AxisToJolt(math::CentimetersToMeters(halfExtentCentimeters));
}

// 길이 스칼라(반지름·반높이·convex radius·여유 거리, cm → m)다.
inline float ToJoltLength(double centimeters) noexcept {
    return static_cast<float>(math::CentimetersToMeters(centimeters));
}

inline double FromJoltLength(float meters) noexcept {
    return math::MetersToCentimeters(static_cast<double>(meters));
}

// ---- 운동량(벡터) ----

// 선속도 cm/s ↔ m/s.
inline JPH::Vec3 ToJoltLinearVelocity(const Vec3d& centimetersPerSecond) noexcept {
    return AxisToJolt(math::CentimetersPerSecondToMetersPerSecond(centimetersPerSecond));
}

inline Vec3d FromJoltLinearVelocity(JPH::Vec3Arg metersPerSecond) noexcept {
    return math::MetersPerSecondToCentimetersPerSecond(AxisFromJolt(metersPerSecond));
}

// 선가속도·중력 cm/s² ↔ m/s².
inline JPH::Vec3 ToJoltAcceleration(const Vec3d& centimetersPerSecondSquared) noexcept {
    return AxisToJolt(
        math::CentimetersPerSecondSquaredToMetersPerSecondSquared(centimetersPerSecondSquared));
}

inline Vec3d FromJoltAcceleration(JPH::Vec3Arg metersPerSecondSquared) noexcept {
    return math::MetersPerSecondSquaredToCentimetersPerSecondSquared(
        AxisFromJolt(metersPerSecondSquared));
}

// 각속도 rad/s(길이 차원 없음 — 재배치만, 3절). 도/s API는 공개 API 층에서 rad/s로 바꾼다.
inline JPH::Vec3 ToJoltAngularVelocity(const Vec3d& radiansPerSecond) noexcept {
    return AxisToJolt(radiansPerSecond);
}

inline Vec3d FromJoltAngularVelocity(JPH::Vec3Arg radiansPerSecond) noexcept {
    return AxisFromJolt(radiansPerSecond);
}

// 힘 kg·cm/s² → N.
inline JPH::Vec3 ToJoltForce(const Vec3d& kilogramCentimetersPerSecondSquared) noexcept {
    return AxisToJolt(
        math::KilogramCentimetersPerSecondSquaredToNewtons(kilogramCentimetersPerSecondSquared));
}

// 선형 충격량 kg·cm/s ↔ N·s.
inline JPH::Vec3 ToJoltLinearImpulse(const Vec3d& kilogramCentimetersPerSecond) noexcept {
    return AxisToJolt(
        math::KilogramCentimetersPerSecondToNewtonSeconds(kilogramCentimetersPerSecond));
}

inline Vec3d FromJoltLinearImpulse(JPH::Vec3Arg newtonSeconds) noexcept {
    return math::NewtonSecondsToKilogramCentimetersPerSecond(AxisFromJolt(newtonSeconds));
}

// 토크 kg·cm²/s² → N·m(배율 10⁻⁴).
inline JPH::Vec3 ToJoltTorque(const Vec3d& kilogramCentimetersSquaredPerSecondSquared) noexcept {
    return AxisToJolt(math::KilogramCentimetersSquaredPerSecondSquaredToNewtonMeters(
        kilogramCentimetersSquaredPerSecondSquared));
}

// 각충격량·각운동량 kg·cm²/s → kg·m²/s.
inline JPH::Vec3 ToJoltAngularImpulse(const Vec3d& kilogramSquareCentimetersPerSecond) noexcept {
    return AxisToJolt(math::KilogramSquareCentimetersPerSecondToKilogramSquareMetersPerSecond(
        kilogramSquareCentimetersPerSecond));
}

// ---- 방향·회전 ----

// 단위 법선(접촉·hit·평면 — 크기 없음). 부호 규약은 어댑터 경계 5절(질의 결과 쪽이 맞춘다).
inline JPH::Vec3 ToJoltNormal(const Vec3d& unit) noexcept {
    return AxisToJolt(unit);
}

inline Vec3d FromJoltNormal(JPH::Vec3Arg unit) noexcept {
    return AxisFromJolt(unit);
}

// 회전(단위 quaternion): (A·v, w). 회전 행렬은 A·R·Aᵀ다.
inline JPH::Quat ToJoltRotation(const Quatd& rotation) noexcept {
    return JPH::Quat(static_cast<float>(rotation.y), static_cast<float>(rotation.z),
                     static_cast<float>(rotation.x), static_cast<float>(rotation.w));
}

inline Quatd FromJoltRotation(JPH::QuatArg rotation) noexcept {
    return Quatd(static_cast<double>(rotation.GetZ()), static_cast<double>(rotation.GetX()),
                 static_cast<double>(rotation.GetY()), static_cast<double>(rotation.GetW()));
}

// ---- 질량 성질 ----

// 관성 텐서(kg·cm²)를 주축 값과 주축 회전(Rocks 좌표 — I = R·D·Rᵀ)으로 받아 Jolt 관성 텐서
// (kg·m², Mat44 — 3×3 부분만 쓰고 [3][3] = 1)로 바꾼다. 결과는 A·I·Aᵀ = (A·R·Aᵀ)·(A·D·Aᵀ)·(A·R·Aᵀ)ᵀ
// 다: 주축 회전은 ToJoltRotation으로, 주축 값(대각)은 크기를 바꾼 뒤 A로 재배치한다.
inline JPH::Mat44 ToJoltInertia(const Vec3d& principalKilogramSquareCentimeters,
                                const Quatd& principalRotation) noexcept {
    const JPH::Mat44 rotation = JPH::Mat44::sRotation(ToJoltRotation(principalRotation));
    const JPH::Mat44 diagonal = JPH::Mat44::sScale(AxisToJolt(
        math::KilogramSquareCentimetersToKilogramSquareMeters(principalKilogramSquareCentimeters)));
    return rotation * diagonal * rotation.Transposed3x3();
}

// 밀도 g/cm³ → kg/m³(×1000 — 어댑터 경계 3절 K1).
inline float ToJoltDensity(double gramsPerCubicCentimeter) noexcept {
    return static_cast<float>(
        math::GramsPerCubicCentimeterToKilogramsPerCubicMeter(gramsPerCubicCentimeter));
}

} // namespace rocks::physics
