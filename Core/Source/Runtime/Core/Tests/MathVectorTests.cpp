#include <TestSupport/Test.h>
#include <TestSupport/MathTestSupport.h>
#include <Core/Math/IntVector.h>
#include <Core/Math/Vector2.h>
#include <Core/Math/Vector4.h>

#include <float.h>
#include <Platform/Memory.h>
#include <Platform/PlatformMath.h>

namespace rocks::test {
namespace {

constexpr float InfinityF = platform::PlatformMath::Infinity<float>();
const float NaNF = platform::PlatformMath::QuietNaN<float>();

} // namespace

TEST(MathUtility_ConstantsAndAngleConversion) {
    CHECK(math::Pi<double> == 3.141592653589793);
    CHECK(math::Pi<float> == 3.14159265f);
    CHECK(math::DegreesToRadians(180.0) == math::Pi<double>);
    CHECK(Near(math::RadiansToDegrees(math::HalfPi<double>), 90.0, 1.0e-12));
    static_assert(math::TwoPi<double> == 2.0 * math::Pi<double>);
}

TEST(MathUtility_ClampLerpSignContracts) {
    CHECK(math::Clamp(5.0f, 0.0f, 1.0f) == 1.0f);
    CHECK(math::Clamp(-5.0, 0.0, 1.0) == 0.0);
    CHECK(math::IsNaN(math::Clamp(NaNF, 0.0f, 1.0f))); // NaN을 숨기지 않는다.
    // t=1에서 a + (b-a)*1의 반올림 오차가 없도록 b를 정확히 반환한다.
    const float a = 0.1f;
    const float b = 1.0e8f;
    CHECK(math::Lerp(a, b, 1.0f) == b);
    CHECK(math::Lerp(a, b, 0.0f) == a);
    CHECK(math::Lerp(2.0, 4.0, 1.5) == 5.0); // 외삽
    CHECK(math::Sign(-0.0f) == 0.0f && math::Sign(3.0) == 1.0 && math::Sign(-2.0f) == -1.0f);
    CHECK(math::IsNaN(math::Sign(NaNF)));
    CHECK(math::IsNearlyEqual(1.0, 1.0 + 1.0e-10, 1.0e-9));
    CHECK(!math::IsNearlyEqual(1.0f, NaNF, 1.0f));
}

TEST(MathUtility_UnwindRadiansRange) {
    const double inputs[] = {0.0,
                             3.5,
                             -3.5,
                             7.0,
                             -7.0,
                             100.0,
                             -100.0,
                             math::Pi<double>,
                             -math::Pi<double>,
                             3.0 * math::Pi<double>};
    for (double value : inputs) {
        const double wrapped = math::UnwindRadians(value);
        CHECK(wrapped > -math::Pi<double> && wrapped <= math::Pi<double> + 1.0e-12);
        // 같은 각인지 sin/cos로 확인한다.
        CHECK(Near(math::Sin(wrapped), math::Sin(value), 1.0e-9));
        CHECK(Near(math::Cos(wrapped), math::Cos(value), 1.0e-9));
    }
    CHECK(math::IsNaN(math::UnwindRadians(static_cast<double>(InfinityF))));
}

TEST(MathUtility_UnwindDegreesMatchesUnreal) {
    // UE UnwindDegrees(UnrealMathUtility.h:949-964, ±360 반복 루프) 값과 정확히 같다(UE 식 probe).
    // NormalizeAxis와 달리 -180은 그대로이고, -180보다 작은 쪽에서 감긴 -540도 -180이다.
    CHECK(math::UnwindDegrees(0.0) == 0.0);
    CHECK(math::UnwindDegrees(180.0) == 180.0);
    CHECK(math::UnwindDegrees(-180.0) == -180.0);
    CHECK(math::UnwindDegrees(190.0) == -170.0);
    CHECK(math::UnwindDegrees(-190.0) == 170.0);
    CHECK(math::UnwindDegrees(540.0) == 180.0);
    CHECK(math::UnwindDegrees(-540.0) == -180.0);
    CHECK(math::UnwindDegrees(-180.0f) == -180.0f);
    CHECK(math::FindDeltaAngleDegrees(90.0, -90.0) == -180.0); // UE Wrap(-180, -180, 180) = -180
    CHECK(math::FindDeltaAngleDegrees(-90.0, 90.0) == 180.0);
    CHECK(math::UnwindDegrees(725.0) == 5.0);
    CHECK(math::UnwindDegrees(359.5) == -0.5);
    CHECK(math::UnwindDegrees(-1.0e-20) == -1.0e-20);
    CHECK(math::UnwindDegrees(370.0f) == 10.0f);
    CHECK(math::IsNaN(math::UnwindDegrees(static_cast<double>(InfinityF))));
    CHECK(math::IsNaN(math::UnwindDegrees(NaNF)));
    // 최소 각 차이(UE FindDeltaAngleDegrees 대응).
    CHECK(math::FindDeltaAngleDegrees(170.0, -170.0) == 20.0);
    CHECK(math::FindDeltaAngleDegrees(-170.0, 170.0) == -20.0);
    CHECK(math::FindDeltaAngleDegrees(10.0, 190.0) == 180.0);
}

TEST(MathUtility_FastAsinMatchesUnrealBits) {
    // UE FMath::FastAsin(float)(UnrealMathUtility.h:819-847)을 옮긴 probe의 bit 값과 정확히
    // 같다. 사칙·sqrt만 쓰므로 IEEE 플랫폼에서 결정적이다.
    struct Case {
        float input;
        uint32 bits;
    };

    const Case cases[] = {{0.5f, 0x3f060a92u},  {-0.5f, 0xbf060a92u}, {0.25f, 0x3e815f50u},
                          {0.9f, 0x3f8f549bu},  {-0.9f, 0xbf8f549bu}, {1.0f, 0x3fc90fdau},
                          {-1.0f, 0xbfc90fdau}, {1.5f, 0x3fc90fdau},  {0.999f, 0x3fc3564fu}};
    for (const Case& item : cases) {
        CHECK(platform::BitCast<uint32>(math::FastAsin(item.input)) == item.bits);
    }

    CHECK(math::FastAsin(0.0f) == 0.0f);
    CHECK(math::IsNaN(math::FastAsin(NaNF)));
    // 근사 오차: [-1, 1] 전 구간에서 2.3e-7 rad 이하(1e-6 간격 전수 최대 2.22e-7).
    for (int step = -10000; step <= 10000; ++step) {
        const float value = static_cast<float>(step) / 10000.0f;
        CHECK(Near(static_cast<double>(math::FastAsin(value)),
                   math::Asin(static_cast<double>(value)), 2.3e-7));
    }

    // double은 UE와 같이 근사 없는 Asin이다.
    CHECK(math::FastAsin(0.3) == math::Asin(0.3));
}

TEST(MathUtility_ConversionsForwardToPlatform) {
    CHECK(math::FloorToInt32(-1.5f) == -2);
    CHECK(math::CeilToInt32(-1.5) == -1);
    CHECK(math::RoundToInt32(-1.5f) == -2);
    CHECK(math::TruncToInt32(-1.9) == -1);
    int32 output = 9;
    CHECK(!math::TryFloorToInt32(static_cast<double>(NaNF), output) && output == 9);
    CHECK(math::TryRoundToInt32(1.5, output) && output == 2);
}

TEST(Vector3_AxisConventionMatchesUnreal) {
    // UE 5.8 방향 상수(Vector.h:84-100, UnrealMath.cpp:35-51)와 정확히 같다.
    CHECK(Vec3::Forward() == Vec3(1.0f, 0.0f, 0.0f));
    CHECK(Vec3::Backward() == Vec3(-1.0f, 0.0f, 0.0f));
    CHECK(Vec3::Right() == Vec3(0.0f, 1.0f, 0.0f));
    CHECK(Vec3::Left() == Vec3(0.0f, -1.0f, 0.0f));
    CHECK(Vec3::Up() == Vec3(0.0f, 0.0f, 1.0f));
    CHECK(Vec3d::Down() == Vec3d(0.0, 0.0, -1.0));
    // UE operator^(Vector.h:1521-1530) golden(UE 식 probe): X^Y = Z, Y^Z = X, Z^X = Y,
    // Forward^Right = Up, Right^Forward = Down. 축 단위 벡터라 반올림 없이 정확하다.
    CHECK(Cross(Vec3::UnitX(), Vec3::UnitY()) == Vec3::UnitZ());
    CHECK(Cross(Vec3::UnitY(), Vec3::UnitZ()) == Vec3::UnitX());
    CHECK(Cross(Vec3::UnitZ(), Vec3::UnitX()) == Vec3::UnitY());
    CHECK(Cross(Vec3::Forward(), Vec3::Right()) == Vec3::Up());
    CHECK(Cross(Vec3::Right(), Vec3::Forward()) == Vec3::Down());
    CHECK(Cross(Vec3d::Right(), Vec3d::Up()) == Vec3d::Forward());
    CHECK(Cross(Vec3d::Up(), Vec3d::Forward()) == Vec3d::Right());
    CHECK(Cross(Vec3d::Forward(), Vec3d::Left()) == Vec3d::Down());
    // 외적은 반대칭이다.
    const Vec3d a(1.25, -2.0, 3.5);
    const Vec3d b(-0.5, 4.0, 0.75);
    CHECK(Cross(a, b) == -Cross(b, a));
    CHECK(Near(Dot(Cross(a, b), a), 0.0, 1.0e-12));
}

TEST(Vector3_ArithmeticAndComparison) {
    const Vec3 a(1.0f, 2.0f, 3.0f);
    const Vec3 b(4.0f, -5.0f, 6.0f);
    CHECK(a + b == Vec3(5.0f, -3.0f, 9.0f));
    CHECK(a - b == Vec3(-3.0f, 7.0f, -3.0f));
    CHECK(a * b == Vec3(4.0f, -10.0f, 18.0f));
    CHECK(2.0f * a == Vec3(2.0f, 4.0f, 6.0f));
    CHECK(a / 2.0f == Vec3(0.5f, 1.0f, 1.5f));
    CHECK(Dot(a, b) == 12.0f);
    CHECK(a[0] == 1.0f && a[2] == 3.0f);
    Vec3 c = a;
    c += b;
    c -= a;
    c *= 2.0f;
    CHECK(c == b * 2.0f);
    CHECK(Vec3(NaNF, 0.0f, 0.0f) != Vec3(NaNF, 0.0f, 0.0f));
    CHECK(a.Equals(Vec3(1.0f, 2.0f, 3.001f), 0.002f));
    CHECK(!a.Equals(Vec3(1.0f, 2.0f, 3.001f), 0.0005f));
    CHECK(ComponentMin(a, b) == Vec3(1.0f, -5.0f, 3.0f));
    CHECK(ComponentMax(a, b) == Vec3(4.0f, 2.0f, 6.0f));
    CHECK(b.MaxAbsComponent() == 6.0f && b.MinComponent() == -5.0f);
    CHECK(Vec3d(a) == Vec3d(1.0, 2.0, 3.0));
    CHECK(Vec3(Vec3d(0.1, 0.2, 0.3)) == Vec3(0.1f, 0.2f, 0.3f));
}

TEST(Vector3_TryNormalizeContract) {
    auto unit = Vec3(3.0f, 0.0f, 4.0f).TryNormalize();
    REQUIRE(unit);
    CHECK(unit.Value().Equals(Vec3(0.6f, 0.0f, 0.8f), 1.0e-7f));
    CHECK(unit.Value().IsNormalized(1.0e-6f));
    // 실패: 영벡터·NaN·Inf. 오류 종류는 InvalidArgument다.
    CHECK(Vec3::Zero().TryNormalize().Error() == ErrorCode::InvalidArgument);
    CHECK(!Vec3(NaNF, 1.0f, 0.0f).TryNormalize());
    CHECK(!Vec3(InfinityF, 1.0f, 0.0f).TryNormalize());
    CHECK(!Vec3(-0.0f, 0.0f, -0.0f).TryNormalize());
    // 제곱 길이가 overflow/underflow하는 유한 입력도 정규화한다.
    auto huge = Vec3(3.0e30f, 4.0e30f, 0.0f).TryNormalize();
    REQUIRE(huge);
    CHECK(huge.Value().Equals(Vec3(0.6f, 0.8f, 0.0f), 1.0e-6f));
    auto tiny = Vec3(1.0e-40f, 0.0f, 0.0f).TryNormalize(); // subnormal
    REQUIRE(tiny);
    CHECK(tiny.Value() == Vec3::UnitX());
    constexpr float Highest = NumericLimits<float>::Max();
    auto maximum = Vec3(Highest, Highest, Highest).TryNormalize();
    REQUIRE(maximum);
    CHECK(maximum.Value().IsNormalized(1.0e-6f));
    // minimumLength는 길이 단위다. 길이 2인 벡터는 2 이하 기준에서 실패한다.
    CHECK(!Vec3d(0.0, 2.0, 0.0).TryNormalize(2.0));
    CHECK(Vec3d(0.0, 2.0, 0.0).TryNormalize(1.999));
    CHECK(Vec3::Zero().NormalizedOr(Vec3::Up()) == Vec3::Up());
}

TEST(Vector3_LengthAndDistance) {
    CHECK(Vec3d(2.0, 3.0, 6.0).Length() == 7.0);
    CHECK(Vec3d(2.0, 3.0, 6.0).LengthSquared() == 49.0);
    CHECK(Distance(Vec3d(1.0, 1.0, 1.0), Vec3d(3.0, 4.0, 7.0)) == 7.0);
    CHECK(DistanceSquared(Vec3(0.0f, 0.0f, 0.0f), Vec3(1.0f, 2.0f, 2.0f)) == 9.0f);
    // 단순 Length는 overflow를 숨기지 않는다(정규화 가능성 판단은 TryNormalize).
    CHECK(math::IsFinite(Vec3(3.0e30f, 0.0f, 0.0f).Length()) == false);
    CHECK(Vec3(1.0f, NaNF, 0.0f).ContainsNaN());
    CHECK(!Vec3(1.0f, InfinityF, 0.0f).IsFinite());
    CHECK(Vec3(1.0e-5f, -1.0e-5f, 0.0f).IsNearlyZero(1.0e-5f));
}

TEST(Vector3_ProjectionReflectionAndAngle) {
    const Vec3d v(3.0, 4.0, 5.0);
    CHECK(ProjectOntoUnit(v, Vec3d::Up()) == Vec3d(0.0, 0.0, 5.0));
    auto projected = TryProjectOnto(v, Vec3d(0.0, 10.0, 0.0));
    REQUIRE(projected);
    CHECK(projected.Value() == Vec3d(0.0, 4.0, 0.0));
    CHECK(!TryProjectOnto(v, Vec3d::Zero()));
    CHECK(Reflect(Vec3d(1.0, -1.0, 0.0), Vec3d::Right()) == Vec3d(1.0, 1.0, 0.0));
    auto right = TryAngleBetween(Vec3d::Forward(), Vec3d::Right());
    REQUIRE(right);
    CHECK(Near(right.Value(), math::HalfPi<double>, 1.0e-15));
    auto opposite = TryAngleBetween(Vec3d::Up(), Vec3d::Down());
    REQUIRE(opposite);
    CHECK(Near(opposite.Value(), math::Pi<double>, 1.0e-15));
    // 거의 평행한 벡터에서도 acos(dot)보다 안정적인 작은 각을 낸다.
    auto small = TryAngleBetween(Vec3d(1.0, 0.0, 0.0), Vec3d(1.0, 1.0e-9, 0.0));
    REQUIRE(small);
    CHECK(Near(small.Value(), 1.0e-9, 1.0e-18, 1.0e-6));
    CHECK(!TryAngleBetween(Vec3d::Zero(), Vec3d::Up()));
}

TEST(Vector3_LerpEndpointsAreExact) {
    const Vec3 a(0.1f, -7.3f, 1.0e7f);
    const Vec3 b(3.3f, 2.9f, -1.0e-7f);
    CHECK(Lerp(a, b, 0.0f) == a);
    CHECK(Lerp(a, b, 1.0f) == b);
    CHECK(Lerp(Vec3d(0.0, 0.0, 0.0), Vec3d(2.0, 4.0, 6.0), 0.5) == Vec3d(1.0, 2.0, 3.0));
}

TEST(Vector3_RandomizedDotCrossProperties) {
    MathRandom random(0x51ed270b2c8f4a11ull ^ CurrentSeed());
    for (int iteration = 0; iteration < 20000; ++iteration) {
        const Vec3d a{random.Range(-100.0, 100.0), random.Range(-100.0, 100.0),
                      random.Range(-100.0, 100.0)};
        const Vec3d b{random.Range(-100.0, 100.0), random.Range(-100.0, 100.0),
                      random.Range(-100.0, 100.0)};
        const Vec3d cross = Cross(a, b);
        // |a × b|² + (a·b)² = |a|²|b|² (Lagrange 항등식)
        const double lhs = cross.LengthSquared() + Dot(a, b) * Dot(a, b);
        CHECK(Near(lhs, a.LengthSquared() * b.LengthSquared(), 1.0e-6, 1.0e-12));
        CHECK(Near(Dot(cross, b), 0.0, 1.0e-8));
        auto normalized = a.TryNormalize();
        if (normalized) {
            CHECK(normalized.Value().IsNormalized(1.0e-14));
        }
    }
}

TEST(Vector2_And_Vector4_BasicContracts) {
    const Vec2 a(3.0f, 4.0f);
    CHECK(a.Length() == 5.0f);
    CHECK(Cross(Vec2::UnitX(), Vec2::UnitY()) == 1.0f); // 반시계 방향이 양수
    auto unit = a.TryNormalize();
    REQUIRE(unit);
    CHECK(unit.Value().Equals(Vec2(0.6f, 0.8f), 1.0e-7f));
    CHECK(!Vec2(NaNF, 0.0f).TryNormalize());
    CHECK(!Vec2::Zero().TryNormalize());
    CHECK(Lerp(Vec2d(0.0, 0.0), Vec2d(2.0, 2.0), 1.0) == Vec2d(2.0, 2.0));

    const Vec4 p = Vec4::Point(Vec3(1.0f, 2.0f, 3.0f));
    CHECK(p.w == 1.0f && Vec4::Direction(Vec3::Up()).w == 0.0f);
    CHECK(p.Xyz() == Vec3(1.0f, 2.0f, 3.0f));
    CHECK(Dot(Vec4(1.0f, 2.0f, 3.0f, 4.0f), Vec4(5.0f, 6.0f, 7.0f, 8.0f)) == 70.0f);
    CHECK(p[3] == 1.0f);
    // float Dot은 (x+y)+(z+w) 순서다. 순서에 민감한 입력으로 platform simd Dot4와 같은지 확인한다.
    const Vec4 big(1.0e8f, 1.0f, -1.0e8f, 1.0f);
    const Vec4 ones(1.0f, 1.0f, 1.0f, 1.0f);
    const float simd = platform::simd::Dot4(platform::simd::Set(big.x, big.y, big.z, big.w),
                                            platform::simd::Set(1.0f, 1.0f, 1.0f, 1.0f));
    CHECK(Dot(big, ones) == simd);
}

TEST(IntVector_CheckedArithmetic) {
    const IntVec2 a(3, -4);
    CHECK(a + IntVec2(1, 1) == IntVec2(4, -3));
    CHECK(a - IntVec2(5, 5) == IntVec2(-2, -9));
    CHECK(a * -2 == IntVec2(-6, 8));
    CHECK(IntVec3(7, -7, 9) / 2 == IntVec3(3, -3, 4)); // 0 방향 절사
    CHECK(IntVec3(NumericLimits<int32>::Max() - 1, 0, 0) + IntVec3(1, 0, 0) ==
          IntVec3(NumericLimits<int32>::Max(), 0, 0));
    // overflow·0 나눗셈은 Fatal이다. 프로세스 음성 대조(math-int-overflow)에서 확인한다.
}

TEST(MathUtility_LerpOverflowAndNaNPropagation) {
    const float lowest = NumericLimits<float>::Lowest();
    const float highest = NumericLimits<float>::Max();
    CHECK(math::Lerp(lowest, highest, 0.0f) == lowest);
    CHECK(math::Lerp(lowest, highest, 1.0f) == highest);
    CHECK(math::Lerp(lowest, highest, 0.5f) == 0.0f);
    CHECK(math::IsFinite(math::Lerp(lowest, highest, 0.75f)));
    CHECK(math::IsNaN(math::MinPropagateNaN(1.0, platform::PlatformMath::QuietNaN<double>())));
    CHECK(math::IsNaN(math::MaxPropagateNaN(platform::PlatformMath::QuietNaN<double>(), 1.0)));
    CHECK(math::MinPropagateNaN(2.0f, -1.0f) == -1.0f &&
          math::MaxPropagateNaN(2.0f, -1.0f) == 2.0f);
}

TEST(MathUtility_NamedTolerancesAndUlpComparison) {
    // UE 5.8 UE_SMALL_NUMBER·UE_KINDA_SMALL_NUMBER와 같은 값이다.
    CHECK(math::SmallNumber<float> == 1.0e-8f && math::KindaSmallNumber<double> == 1.0e-4);
    CHECK(math::MachineEpsilon<float> == FLT_EPSILON &&
          math::MachineEpsilon<double> == DBL_EPSILON);
    CHECK(math::IsNearlyEqual(1.0f, 1.00005f, math::KindaSmallNumber<float>));
    // 상대 비교: 큰 값에서는 절대 오차 대신 비율로 판정한다.
    CHECK(math::IsNearlyEqualRelative(1.0e6, 1.0e6 + 0.5, 1.0e-6, 1.0e-9));
    CHECK(!math::IsNearlyEqualRelative(1.0, 1.1, 1.0e-6, 1.0e-9));
    CHECK(math::IsNearlyEqualRelative(0.0, 1.0e-10, 1.0e-6, 1.0e-9));
    CHECK(!math::IsNearlyEqualRelative(platform::PlatformMath::QuietNaN<double>(), 0.0, 1.0, 1.0));
    // ULP 비교: 0을 사이에 둔 값, 부호 있는 0, 무한대, NaN
    const float one = 1.0f;
    const float next = platform::BitCast<float>(platform::BitCast<uint32>(one) + 4u);
    CHECK(math::IsNearlyEqualByUlps(one, next, 4) && !math::IsNearlyEqualByUlps(one, next, 3));
    CHECK(math::IsNearlyEqualByUlps(0.0f, -0.0f, 0));
    const float tinyPositive = platform::BitCast<float>(1u);
    const float tinyNegative = platform::BitCast<float>(0x80000001u);
    CHECK(math::IsNearlyEqualByUlps(tinyPositive, tinyNegative, 2));
    CHECK(!math::IsNearlyEqualByUlps(tinyPositive, tinyNegative, 1));
    CHECK(!math::IsNearlyEqualByUlps(NumericLimits<float>::Max(), InfinityF, 1000000));
    CHECK(math::IsNearlyEqualByUlps(InfinityF, InfinityF, 0));
    CHECK(!math::IsNearlyEqualByUlps(NaNF, NaNF, 1000000));
    const double d = 1.0;
    const double dNext = platform::BitCast<double>(platform::BitCast<uint64>(d) + 2ull);
    CHECK(math::IsNearlyEqualByUlps(d, dNext, 2) && !math::IsNearlyEqualByUlps(d, dNext, 1));
    CHECK(math::IsNearlyEqualByUlps(-0.0, 0.0, 0));
}

} // namespace rocks::test
