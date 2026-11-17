#include <TestSupport/Test.h>
#include <Core/Math/UnitConversion.h>

namespace rocks::test {

// 단위 경계 변환 golden이다. 기대값은 UE 5.8.4 계수(UnrealMathUtility.h:177-183,
// UnitConversion.cpp:723·:752-753·:775·:818·:828, PrimitiveComponent.h:2863)와 UE 기본값
// (PhysicsSettingsCore.cpp:29 중력 -980, Chaos/PerParticleGravity.h:17 -980.665,
// CharacterMovementComponent.cpp:694 걷기 속도 600)에서 정한 구체 값이다. 각 기대값은 정확한 값을
// T로 한 번 반올림한 값과 같으므로(Python Fraction으로 확인) 모두 정확 일치(==)로 비교한다.

TEST(MathUnitConversion_FactorsMatchUe) {
    CHECK(math::CentimetersPerMeter<double> == 100.0);
    CHECK(math::CentimetersPerMeter<float> == 100.0f);
    CHECK(math::SquareCentimetersPerSquareMeter<double> == 10000.0);
    CHECK(math::SquareCentimetersPerSquareMeter<float> == 10000.0f);
}

TEST(MathUnitConversion_LengthGolden) {
    CHECK(math::CentimetersToMeters(250.0) == 2.5);
    CHECK(math::CentimetersToMeters(-250.0f) == -2.5f);
    CHECK(math::MetersToCentimeters(2.5) == 250.0);
    CHECK(math::MetersToCentimeters(6.0f) == 600.0f);
    // 1 cm = 0.01 m: 나눗셈 한 번이라 double 0.01과 비트 단위로 같다. UE UE_CM_TO_M(float
    // 0.01f)을 double에 곱한 값과는 다르다(0.0099999997764825820…).
    CHECK(math::CentimetersToMeters(1.0) == 0.01);
    CHECK(math::CentimetersToMeters(1.0) != 1.0 * static_cast<double>(0.01f));
    CHECK(math::CentimetersToMeters(Vec3d(100.0, -250.0, 980.0)) == Vec3d(1.0, -2.5, 9.8));
    CHECK(math::MetersToCentimeters(Vec3(1.0f, -2.5f, 0.0f)) == Vec3(100.0f, -250.0f, 0.0f));
}

TEST(MathUnitConversion_AreaGolden) {
    CHECK(math::SquareCentimetersToSquareMeters(10000.0) == 1.0);
    CHECK(math::SquareCentimetersToSquareMeters(2500.0f) == 0.25f);
    CHECK(math::SquareMetersToSquareCentimeters(0.25) == 2500.0);
}

TEST(MathUnitConversion_SpeedGolden) {
    // UE CharacterMovement MaxWalkSpeed 기본 600 cm/s = 6 m/s.
    CHECK(math::CentimetersPerSecondToMetersPerSecond(600.0) == 6.0);
    CHECK(math::CentimetersPerSecondToMetersPerSecond(600.0f) == 6.0f);
    CHECK(math::MetersPerSecondToCentimetersPerSecond(6.0) == 600.0);
    CHECK(math::CentimetersPerSecondToMetersPerSecond(Vec3d(600.0, 0.0, -250.0)) ==
          Vec3d(6.0, 0.0, -2.5));
}

TEST(MathUnitConversion_AccelerationAndGravityGolden) {
    // UE 기본 중력 -980 cm/s² = -9.8 m/s², Chaos 기본 -980.665 cm/s² = -9.80665 m/s².
    CHECK(math::CentimetersPerSecondSquaredToMetersPerSecondSquared(-980.0) == -9.8);
    CHECK(math::CentimetersPerSecondSquaredToMetersPerSecondSquared(-980.0f) == -9.8f);
    CHECK(math::CentimetersPerSecondSquaredToMetersPerSecondSquared(-980.665) == -9.80665);
    CHECK(math::CentimetersPerSecondSquaredToMetersPerSecondSquared(-980.665f) == -9.80665f);
    CHECK(math::CentimetersPerSecondSquaredToMetersPerSecondSquared(Vec3d(0.0, 0.0, 980.0)) ==
          Vec3d(0.0, 0.0, 9.8));
    CHECK(math::MetersPerSecondSquaredToCentimetersPerSecondSquared(-9.80665) == -980.665);
    CHECK(math::MetersPerSecondSquaredToCentimetersPerSecondSquared(-9.8f) == -980.0f);
}

TEST(MathUnitConversion_ForceAndImpulseGolden) {
    // 1 kg에 걸리는 UE 기본 중력 980 kg·cm/s² = 9.8 N(UE kg·cm/s² → N 계수 0.01).
    CHECK(math::KilogramCentimetersPerSecondSquaredToNewtons(980.0) == 9.8);
    CHECK(math::KilogramCentimetersPerSecondSquaredToNewtons(980.0f) == 9.8f);
    CHECK(math::NewtonsToKilogramCentimetersPerSecondSquared(1.0) == 100.0);
    CHECK(math::NewtonsToKilogramCentimetersPerSecondSquared(Vec3d(0.0, 2.5, -1.0)) ==
          Vec3d(0.0, 250.0, -100.0));
    // 선형 충격량: 1 kg이 600 cm/s 변하면 600 kg·cm/s = 6 N·s.
    CHECK(math::KilogramCentimetersPerSecondToNewtonSeconds(600.0) == 6.0);
    CHECK(math::NewtonSecondsToKilogramCentimetersPerSecond(1.0f) == 100.0f);
}

TEST(MathUnitConversion_TorqueAndInertiaUseSquareFactor) {
    // 토크·관성은 길이 2차라 계수가 10000이다. 같은 입력의 힘 변환(100)과 다르다.
    CHECK(math::NewtonMetersToKilogramCentimetersSquaredPerSecondSquared(1.0) == 10000.0);
    CHECK(math::NewtonsToKilogramCentimetersPerSecondSquared(1.0) == 100.0);
    CHECK(math::KilogramCentimetersSquaredPerSecondSquaredToNewtonMeters(10000.0) == 1.0);
    CHECK(math::KilogramCentimetersSquaredPerSecondSquaredToNewtonMeters(25000.0f) == 2.5f);
    // 질량 1 kg·반지름 10 cm 속찬 구: I = 2/5·m·r² = 40 kg·cm² = 0.004 kg·m²(×1e-4).
    CHECK(math::KilogramSquareCentimetersToKilogramSquareMeters(40.0) == 0.004);
    CHECK(math::KilogramSquareCentimetersToKilogramSquareMeters(40.0f) == 0.004f);
    CHECK(math::KilogramSquareMetersToKilogramSquareCentimeters(0.004) == 40.0);
    CHECK(math::KilogramSquareCentimetersToKilogramSquareMeters(Vec3d(40.0, 40.0, 25000.0)) ==
          Vec3d(0.004, 0.004, 2.5));
}

TEST(MathUnitConversion_NonFiniteIsPreserved) {
    const double infinity = platform::PlatformMath::Infinity<double>();
    const double nan = platform::PlatformMath::QuietNaN<double>();
    CHECK(math::CentimetersToMeters(infinity) == infinity);
    CHECK(math::CentimetersToMeters(-infinity) == -infinity);
    CHECK(math::IsNaN(math::MetersToCentimeters(nan)));
    // 범위를 넘으면 검사 없이 ±Inf다.
    CHECK(math::MetersToCentimeters(3.0e38f) == platform::PlatformMath::Infinity<float>());
}

} // namespace rocks::test
