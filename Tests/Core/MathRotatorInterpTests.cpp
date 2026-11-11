#include "Tests/Support/Test.h"
#include "Tests/Support/MathTestSupport.h"
#include <Core/Math/Rotator.h>

namespace rocks::test {

TEST(Rotator_AxisMeaningsInRocksConvention) {
    const double quarter = math::HalfPi<double>;
    // 양의 yaw: 전방이 왼쪽으로 돈다.
    CHECK(Rotator<double>(quarter, 0.0, 0.0)
              .ToQuaternion()
              .GetForward()
              .Equals(Vec3d::Left(), 1.0e-15));
    // 양의 pitch: 기수가 위로 든다.
    CHECK(Rotator<double>(0.0, quarter, 0.0)
              .ToQuaternion()
              .GetForward()
              .Equals(Vec3d::Up(), 1.0e-15));
    // 양의 roll: 왼쪽 축이 위로 올라간다.
    CHECK(Rotator<double>(0.0, 0.0, quarter).ToQuaternion().GetLeft().Equals(Vec3d::Up(), 1.0e-15));
    // 적용 순서 roll → pitch → yaw: (yaw 90, pitch 90)의 전방은 위, 왼쪽은 world -X.
    const Quatd combined = Rotator<double>(quarter, quarter, 0.0).ToQuaternion();
    CHECK(combined.GetForward().Equals(Vec3d::Up(), 1.0e-15));
    CHECK(combined.GetLeft().Equals(Vec3d::Backward(), 1.0e-15));
    CHECK(Rotator<double>::FromDegrees(180.0, 0.0, 0.0).yaw == math::Pi<double>);
}

TEST(Rotator_QuaternionRoundTripIncludingGimbalLock) {
    MathRandom random(0x0f1e2d3c4b5a6978ull ^ CurrentSeed());
    for (int iteration = 0; iteration < 5000; ++iteration) {
        const Rotator<double> source(random.Range(-3.1, 3.1), random.Range(-1.5, 1.5),
                                     random.Range(-3.1, 3.1));
        auto recovered = Rotator<double>::TryFromQuaternion(source.ToQuaternion());
        REQUIRE(recovered);
        // 일반 자세에서는 같은 각, 항상 같은 회전이다.
        CHECK(AngularDistance(recovered.Value().ToQuaternion(), source.ToQuaternion()) < 1.0e-6);
        CHECK(Near(recovered.Value().pitch, source.pitch, 1.0e-6));
    }
    // pitch ±90도: yaw/roll이 합쳐지며 roll은 0이 된다. 회전은 보존된다.
    const double gimbalPitches[] = {math::HalfPi<double>, -math::HalfPi<double>};
    for (double pitch : gimbalPitches) {
        const Rotator<double> locked(0.4, pitch, 0.3);
        auto recovered = Rotator<double>::TryFromQuaternion(locked.ToQuaternion());
        REQUIRE(recovered);
        CHECK(recovered.Value().roll == 0.0);
        CHECK(AngularDistance(recovered.Value().ToQuaternion(), locked.ToQuaternion()) < 1.0e-6);
    }
    CHECK(!Rotator<float>::TryFromQuaternion(Quat(0.0f, 0.0f, 0.0f, 0.0f)));
    CHECK(!Rotator<double>(__builtin_nan(""), 0.0, 0.0).IsFinite());
    const Rotator<double> wrapped = Rotator<double>(7.0, -7.0, 4.0).Normalized();
    CHECK(wrapped.yaw <= math::Pi<double> && wrapped.yaw > -math::Pi<double>);
    CHECK(AngularDistance(wrapped.ToQuaternion(), Rotator<double>(7.0, -7.0, 4.0).ToQuaternion()) <
          1.0e-9);
}

TEST(MathInterp_SmoothStepAndCubic) {
    CHECK(math::SmoothStep(0.0, 1.0, -1.0) == 0.0);
    CHECK(math::SmoothStep(0.0, 1.0, 2.0) == 1.0);
    CHECK(math::SmoothStep(0.0, 1.0, 0.5) == 0.5);
    CHECK(math::SmoothStep(2.0f, 4.0f, 3.0f) == 0.5f);
    CHECK(math::IsNaN(math::SmoothStep(0.0, 1.0, static_cast<double>(__builtin_nan("")))));
    CHECK(math::CubicInterp(1.0, 0.0, 5.0, 0.0, 0.0) == 1.0);
    CHECK(math::CubicInterp(1.0, 0.0, 5.0, 0.0, 1.0) == 5.0);
    CHECK(math::CubicInterp(0.0, 0.0, 2.0, 0.0, 0.5) == 1.0);
    // 접선이 직선 기울기와 같으면 선형이다.
    CHECK(Near(math::CubicInterp(0.0, 2.0, 2.0, 2.0, 0.25), 0.5, 1.0e-15));
}

TEST(MathInterp_InterpToContracts) {
    CHECK(math::InterpTo(0.0, 10.0, 0.1, 2.0) == 2.0);
    CHECK(math::InterpTo(0.0, 10.0, 1.0, 2.0) == 10.0); // alpha >= 1
    CHECK(math::InterpTo(3.0, 10.0, 0.1, 0.0) == 10.0); // speed 0 → 즉시 목표
    CHECK(math::InterpTo(3.0, 10.0, 0.0, 5.0) == 3.0);
    CHECK(math::InterpConstantTo(0.0, 10.0, 0.5, 4.0) == 2.0);
    CHECK(math::InterpConstantTo(9.5, 10.0, 0.5, 4.0) == 10.0); // 남은 거리 <= step
    CHECK(math::InterpConstantTo(0.1f, -0.2f, 1.0f, 0.1f) == 0.0f);
    CHECK(math::InterpConstantTo(5.0, 5.0, 0.0, 0.0) == 5.0);
    // 반복 접근은 정확히 목표에 도달한다(누적 반올림으로 멈추지 않는다).
    double value = 0.1;
    for (int frame = 0; frame < 200; ++frame) {
        value = math::InterpConstantTo(value, 0.7, 0.016, 3.0);
    }
    CHECK(value == 0.7);
}

TEST(MathInterp_GridSnapWrapAndDeltaAngle) {
    CHECK(math::GridSnap(7.4, 5.0) == 5.0);
    CHECK(math::GridSnap(7.5, 5.0) == 10.0);
    CHECK(math::GridSnap(-7.5, 5.0) == -5.0); // +무한대 방향
    CHECK(math::GridSnap(3.3, 0.0) == 3.3);
    CHECK(math::Wrap(370.0, 0.0, 360.0) == 10.0);
    CHECK(math::Wrap(-10.0, 0.0, 360.0) == 350.0);
    CHECK(math::Wrap(360.0, 0.0, 360.0) == 0.0);
    CHECK(math::Wrap(-360.0, 0.0, 360.0) == 0.0);
    CHECK(math::Wrap(-1.0e-20, 0.0, 360.0) == 0.0); // 보정 반올림이 max가 되면 min
    CHECK(math::IsNaN(math::Wrap(static_cast<double>(__builtin_huge_val()), 0.0, 1.0)));
    CHECK(Near(math::FindDeltaAngleRadians(3.0, -3.0), 2.0 * math::Pi<double> - 6.0, 1.0e-12));
    CHECK(Near(math::FindDeltaAngleRadians(-3.0, 3.0), 6.0 - 2.0 * math::Pi<double>, 1.0e-12));
}

TEST(Rotator_FromQuaternionUsesHalfOpenRange) {
    // yaw = ±pi는 (-pi, pi] 구간의 +pi로 나온다(atan2(-0, -1) = -pi 보정).
    const Quatd turns[] = {Quatd(0.0, 0.0, 1.0, 0.0), Quatd(0.0, 0.0, -1.0, 0.0),
                           Quatd(1.0, 0.0, 0.0, 0.0), Quatd(-1.0, 0.0, 0.0, 0.0)};
    for (const Quatd& turn : turns) {
        auto rotator = Rotator<double>::TryFromQuaternion(turn);
        REQUIRE(rotator);
        CHECK(rotator.Value().yaw > -math::Pi<double>);
        CHECK(rotator.Value().roll > -math::Pi<double>);
        CHECK(rotator.Value().Normalized().yaw == rotator.Value().yaw);
        auto single = Rotator<float>::TryFromQuaternion(Quat(turn));
        REQUIRE(single);
        CHECK(single.Value().yaw > -math::Pi<float>);
        CHECK(single.Value().roll > -math::Pi<float>);
    }
}

TEST(Interp_PropagatesNaNInsteadOfReturningTarget) {
    const double nan = __builtin_nan("");
    CHECK(math::IsNaN(math::InterpTo(0.0, 1.0, 0.1, nan)));
    CHECK(math::IsNaN(math::InterpTo(nan, 1.0, 0.1, 100.0)));
    CHECK(math::IsNaN(math::InterpTo(0.0, nan, 0.1, 0.0)));
    CHECK(math::IsNaN(math::InterpConstantTo(0.0, nan, 1.0, 1.0)));
    CHECK(math::IsNaN(math::InterpConstantTo(0.0, -nan, 1.0, 1.0)));
    CHECK(math::IsNaN(math::InterpConstantTo(nan, 1.0, 1.0, 1.0)));
    // 유효 입력의 기존 의미는 유지된다.
    CHECK(math::InterpTo(0.0, 1.0, 0.1, 0.0) == 1.0);
    CHECK(math::InterpConstantTo(0.0, 1.0, 1.0, 5.0) == 1.0);
}

TEST(GridSnap_TieDirectionIgnoresGridSign) {
    CHECK(math::GridSnap(7.5, 5.0) == 10.0);
    CHECK(math::GridSnap(7.5, -5.0) == 10.0);
    CHECK(math::GridSnap(-7.5, -5.0) == -5.0);
    CHECK(math::GridSnap(-7.5, 5.0) == -5.0);
    CHECK(math::GridSnap(3.2, 0.0) == 3.2);
}

} // namespace rocks::test
