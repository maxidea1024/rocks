#include <TestSupport/Test.h>
#include <TestSupport/MathTestSupport.h>
#include <Core/Math/Rotator.h>
#include <Platform/PlatformMath.h>

namespace rocks::test {
namespace math_rotator_interp_tests_file {

// Rotator golden은 UE 5.8.4 원본 식(UnrealMath.cpp:518-535 Rotator→Quat, :691-719 Quat→Rotator,
// :640-677 float Quat→Rotator, :371-402 Vector, Rotator.h:688-716 ClampAxis·NormalizeAxis,
// Quat.h:1236-1249 RotateVector)을 그대로 옮긴 독립 probe(double, -ffp-contract=off)의 출력이다.
// probe 출처와 실행 결과는 Docs/Implementation/Math.ko.md 5절에 적었다. 정확 일치(==)는 결과가
// 정확히 표현되는 경우(축 상수, atan2(±0, -1), Fmod 감김, 특이점의 ±90·0,
// ClampAxis·NormalizeAxis)에만 쓰고 그 밖은 반올림 수준의 허용 오차로 비교한다.
namespace {

constexpr double SqrtHalf = 0.70710678118654757;

// Q(+Z, yaw) * Q(+Y, -pitch) * Q(+X, -roll) — 축-각 quaternion을 따로 만들어 곱한 독립
// 기준이다(UE 식 probe가 UE 단일식과 1e-12 안에서 같음을 확인한 분해).
Quatd ComposeAxisAngles(const Rotator<double>& rotator) {
    const Quatd yaw = Quatd::FromUnitAxisAngle(Vec3d::Up(), math::DegreesToRadians(rotator.yaw));
    const Quatd pitch =
        Quatd::FromUnitAxisAngle(Vec3d::Right(), -math::DegreesToRadians(rotator.pitch));
    const Quatd roll =
        Quatd::FromUnitAxisAngle(Vec3d::Forward(), -math::DegreesToRadians(rotator.roll));
    return yaw * pitch * roll;
}

struct AxisGolden {
    Rotator<double> rotator;
    Quatd quaternion;
    Vec3d forward; // 회전된 Forward
    Vec3d right;   // 회전된 Right
    Vec3d up;      // 회전된 Up
};

} // namespace

TEST(Rotator_AxisRotationsMatchUnrealGolden) {
    // 축 ±90°(UE 식 probe). UE 결과에도 2.3e-16 이하의 반올림이 남아 회전 벡터는 1e-15,
    // quaternion 성분은 2 ulp(2.5e-16)로 비교한다.
    const AxisGolden cases[] = {
        // yaw +90: 위에서 볼 때 시계 방향 — Forward → Right, Right → Backward.
        {Rotator<double>(0.0, 90.0, 0.0), Quatd(0.0, 0.0, SqrtHalf, SqrtHalf), Vec3d::Right(),
         Vec3d::Backward(), Vec3d::Up()},
        {Rotator<double>(0.0, -90.0, 0.0), Quatd(0.0, 0.0, -SqrtHalf, SqrtHalf), Vec3d::Left(),
         Vec3d::Forward(), Vec3d::Up()},
        // pitch +90: 기수가 위로 — Forward → Up, Up → Backward.
        {Rotator<double>(90.0, 0.0, 0.0), Quatd(0.0, -SqrtHalf, 0.0, SqrtHalf), Vec3d::Up(),
         Vec3d::Right(), Vec3d::Backward()},
        {Rotator<double>(-90.0, 0.0, 0.0), Quatd(0.0, SqrtHalf, 0.0, SqrtHalf), Vec3d::Down(),
         Vec3d::Right(), Vec3d::Forward()},
        // roll +90: +X로 볼 때 시계 방향 — Right → Down, Up → Right.
        {Rotator<double>(0.0, 0.0, 90.0), Quatd(-SqrtHalf, 0.0, 0.0, SqrtHalf), Vec3d::Forward(),
         Vec3d::Down(), Vec3d::Right()},
        {Rotator<double>(0.0, 0.0, -90.0), Quatd(SqrtHalf, 0.0, 0.0, SqrtHalf), Vec3d::Forward(),
         Vec3d::Up(), Vec3d::Left()},
    };
    for (const AxisGolden& item : cases) {
        const Quatd q = item.rotator.ToQuaternion();
        CHECK(q.Equals(item.quaternion, 2.5e-16));
        CHECK(q.RotateVector(Vec3d::Forward()).Equals(item.forward, 1.0e-15));
        CHECK(q.RotateVector(Vec3d::Right()).Equals(item.right, 1.0e-15));
        CHECK(q.RotateVector(Vec3d::Up()).Equals(item.up, 1.0e-15));
        CHECK(q.GetForwardVector().Equals(item.forward, 1.0e-15));
        CHECK(q.GetRightVector().Equals(item.right, 1.0e-15));
        CHECK(q.GetUpVector().Equals(item.up, 1.0e-15));
        CHECK(item.rotator.Vector().Equals(item.forward, 1.0e-15));
        CHECK(item.rotator.RotateVector(Vec3d::Up()).Equals(item.up, 1.0e-15));
        CHECK(item.rotator.UnrotateVector(item.right).Equals(Vec3d::Right(), 1.0e-15));
        CHECK(AngularDistance(q, ComposeAxisAngles(item.rotator)) < 1.0e-15);
        // float도 같은 회전이다.
        const Rotator<float> single(static_cast<float>(item.rotator.pitch),
                                    static_cast<float>(item.rotator.yaw),
                                    static_cast<float>(item.rotator.roll));
        const Quat singleQuaternion = single.ToQuaternion();
        CHECK(singleQuaternion.RotateVector(Vec3::Forward()).Equals(Vec3(item.forward), 1.0e-6f));
        CHECK(singleQuaternion.RotateVector(Vec3::Right()).Equals(Vec3(item.right), 1.0e-6f));
        CHECK(singleQuaternion.RotateVector(Vec3::Up()).Equals(Vec3(item.up), 1.0e-6f));
    }

    // 이전 Rocks 규약과 대수 부호가 반대였던 roll을 따로 고정한다: UE Roll +90은 +Y를 -Z로 보낸다.
    CHECK(Rotator<double>(0.0, 0.0, 90.0).RotateVector(Vec3d::UnitY()).z < -0.999);
    CHECK(Rotator<double>(0.0, 0.0, 30.0).RotateVector(Vec3d::UnitY()).z < -0.49);
    CHECK(Rotator<double>(0.0, 0.0, -30.0).RotateVector(Vec3d::UnitY()).z > 0.49);
}

TEST(Rotator_GeneralAnglesMatchUnrealGolden) {
    // 임의 각(UE 식 probe). UE double 식과 같은 식이라 차이는 libm sin·cos·atan2의
    // 반올림뿐이다 — 성분 1e-15, 분해 결과 1e-12(도)로 비교한다.
    struct GeneralGolden {
        Rotator<double> rotator;
        Quatd quaternion;
        Vec3d forward;
        Vec3d right;
        Vec3d up;
        Vec3d vector;
        Rotator<double> recovered;
    };

    const GeneralGolden cases[] = {
        {Rotator<double>(30.0, 45.0, 60.0),
         Quatd(-0.36042340565035591, -0.39190383732911993, 0.20056212114657512,
               0.82236317190599928),
         Vec3d(0.61237243569579447, 0.61237243569579469, 0.49999999999999989),
         Vec3d(-0.047367172745376662, 0.65973960844117108, -0.75),
         Vec3d(-0.78914913099243134, 0.43559574039915749, 0.43301270189221941),
         Vec3d(0.61237243569579458, 0.61237243569579458, 0.49999999999999994),
         Rotator<double>(29.999999999999993, 45.000000000000014, 59.999999999999993)},
        // 감긴 입력: Fmod 360 뒤 계산하며 분해는 (-20, 160, 5)다.
        {Rotator<double>(-20.0, -200.0, 725.0),
         Quatd(0.17830666252657573, 0.012179111921821712, -0.97023851707327113,
               -0.16338795340810564),
         Vec3d(-0.88302222155948895, 0.32139380484326957, -0.34202014332566871),
         Vec3d(-0.31270733764599895, -0.94631209182781273, -0.081899608319089337),
         Vec3d(-0.34997982401060751, 0.034633034357898551, 0.93611680666285912),
         Vec3d(-0.88302222155948906, 0.32139380484326963, -0.34202014332566871),
         Rotator<double>(-20.0, 160.0, 5.0)},
    };
    for (const GeneralGolden& item : cases) {
        const Quatd q = item.rotator.ToQuaternion();
        CHECK(q.Equals(item.quaternion, 1.0e-15));
        CHECK(q.RotateVector(Vec3d::Forward()).Equals(item.forward, 1.0e-15));
        CHECK(q.RotateVector(Vec3d::Right()).Equals(item.right, 1.0e-15));
        CHECK(q.RotateVector(Vec3d::Up()).Equals(item.up, 1.0e-15));
        CHECK(item.rotator.Vector().Equals(item.vector, 1.0e-15));
        CHECK(AngularDistance(q, ComposeAxisAngles(item.rotator)) < 1.0e-14);
        auto recovered = Rotator<double>::TryFromQuaternion(q);
        REQUIRE(recovered);
        CHECK(Near(recovered.Value().pitch, item.recovered.pitch, 1.0e-12));
        CHECK(Near(recovered.Value().yaw, item.recovered.yaw, 1.0e-12));
        CHECK(Near(recovered.Value().roll, item.recovered.roll, 1.0e-12));
    }

    // Fmod 360 감김은 bit까지 같은 quaternion을 낸다(UE 식과 같다).
    CHECK(Rotator<double>(0.0, 450.0, 0.0).ToQuaternion() ==
          Rotator<double>(0.0, 90.0, 0.0).ToQuaternion());
    CHECK(Rotator<double>(-380.0, 720.0, 1090.0).ToQuaternion() ==
          Rotator<double>(-20.0, 0.0, 10.0).ToQuaternion());
    // float 경로(UE FRotator3f 스칼라 식을 CRT sin·cos로 계산한 probe 값).
    CHECK(Rotator<float>(30.0f, 45.0f, 60.0f)
              .ToQuaternion()
              .Equals(Quat(-0.360423386f, -0.391903818f, 0.200562119f, 0.822363079f), 2.0e-7f));
}

TEST(Rotator_FromQuaternionMatchesUnrealGolden) {
    // UE FQuat::Rotator(UnrealMath.cpp:640-719) probe 값. 아래 입력은 asin·atan2 인자가
    // 정확한 값이라 float·double 모두 결과가 정확히 같다. yaw·roll의 -180은 UE와 같이 그대로 나온다
    // (atan2(-0, 음수) = -pi).
    struct SpecialGolden {
        Quatd input;
        Rotator<double> expected;
    };

    const SpecialGolden cases[] = {
        {Quatd(0.0, 0.0, 0.0, 1.0), Rotator<double>(0.0, 0.0, 0.0)},
        {Quatd(0.0, 0.0, 1.0, 0.0), Rotator<double>(0.0, 180.0, 0.0)},
        {Quatd(0.0, 0.0, -1.0, 0.0), Rotator<double>(0.0, 180.0, 0.0)},
        {Quatd(1.0, 0.0, 0.0, 0.0), Rotator<double>(0.0, 0.0, -180.0)},
        {Quatd(-1.0, 0.0, 0.0, 0.0), Rotator<double>(0.0, 0.0, -180.0)},
        {Quatd(0.0, 1.0, 0.0, 0.0), Rotator<double>(0.0, 180.0, -180.0)},
        // 비단위 입력, 특이점(s = 0.5 > 0.4999995): pitch 90, roll 0,
        // yaw = NormalizeAxis(-180) = 180.
        {Quatd(-1.0, -0.0, -1.0, -0.0), Rotator<double>(90.0, 180.0, 0.0)},
    };
    for (const SpecialGolden& item : cases) {
        auto precise = Rotator<double>::TryFromQuaternion(item.input);
        REQUIRE(precise);
        CHECK(precise.Value().pitch == item.expected.pitch);
        CHECK(precise.Value().yaw == item.expected.yaw);
        CHECK(precise.Value().roll == item.expected.roll);
        auto single = Rotator<float>::TryFromQuaternion(Quat(item.input));
        REQUIRE(single);
        CHECK(single.Value().pitch == static_cast<float>(item.expected.pitch));
        CHECK(single.Value().yaw == static_cast<float>(item.expected.yaw));
        CHECK(single.Value().roll == static_cast<float>(item.expected.roll));
    }

    // 비단위 입력은 정규화 뒤 UE 식이다(이전 Rocks 회귀 입력 — 지금은 UE와 같이 roll -180).
    auto scaled = Rotator<double>::TryFromQuaternion(Quatd(-1.0, -1.0, -0.5, 0.5));
    REQUIRE(scaled);
    CHECK(Near(scaled.Value().pitch, 53.130102354155987, 1.0e-12));
    CHECK(Near(scaled.Value().yaw, 90.0, 1.0e-12));
    CHECK(scaled.Value().roll == -180.0);
    CHECK(AngularDistance(scaled.Value().ToQuaternion(),
                          Quatd(-1.0, -1.0, -0.5, 0.5).TryNormalize().Value()) < 1.0e-12);
    // 실패: 영·비유한 quaternion.
    CHECK(Rotator<float>::TryFromQuaternion(Quat(0.0f, 0.0f, 0.0f, 0.0f)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(!Rotator<double>::TryFromQuaternion(
        Quatd(platform::PlatformMath::QuietNaN<double>(), 0.0, 0.0, 1.0)));
}

TEST(Rotator_FloatPitchUsesUnrealFastAsin) {
    // UE FQuat4f::Rotator는 pitch에 FastAsin을 쓰고(UnrealMath.cpp:672) double은 Asin을 쓴다(:716).
    // 정규화한 Quat(0, -1, 0, 3)은 2s = 0.6이며 FastAsin 결과 0x1.26f58cp+5(36.8698959)가
    // asinf 결과 36.8699036과 다르다(UE 식 probe로 고른 입력). 정규화·FastAsin이 사칙·sqrt뿐이라
    // bit가 결정적이다.
    auto single = Rotator<float>::TryFromQuaternion(Quat(0.0f, -1.0f, 0.0f, 3.0f));
    REQUIRE(single);
    CHECK(single.Value().pitch == 0x1.26f58cp+5f);
    CHECK(single.Value().yaw == 0.0f && single.Value().roll == 0.0f);
    auto precise = Rotator<double>::TryFromQuaternion(Quatd(0.0, -1.0, 0.0, 3.0));
    REQUIRE(precise);
    CHECK(Near(precise.Value().pitch, 36.869897645844021, 1.0e-12));
}

TEST(Rotator_FromQuaternionSingularityMatchesUnreal) {
    // 특이점 임계 0.4999995(UE 식 probe). 바깥은 pitch ±90·roll 0으로 yaw에 합치고 안쪽은
    // 일반 식이다.
    struct SingularGolden {
        Rotator<double> source;
        Rotator<double> expected;
        bool singular;
    };

    const SingularGolden cases[] = {
        {Rotator<double>(90.0, 40.0, 10.0), Rotator<double>(90.0, 29.999999999999993, 0.0), true},
        {Rotator<double>(-90.0, 40.0, 10.0), Rotator<double>(-90.0, 49.999999999999993, 0.0), true},
        // s = 0.49999999238 > 임계: UE와 같이 pitch 90으로 맞추고 roll을 버린다
        // (회전이 약 1.7e-4 rad 달라진다).
        {Rotator<double>(89.99, 40.0, 10.0), Rotator<double>(90.0, 29.993572553568285, 0.0), true},
        // s = 0.49999695 < 임계: 일반 식.
        {Rotator<double>(89.8, 40.0, 10.0),
         Rotator<double>(89.799999999998377, 39.999999999999062, 9.9999999999999627), false},
        {Rotator<double>(-89.8, -130.0, -170.0),
         Rotator<double>(-89.799999999998377, -130.00000000000131, -169.99999999999761), false},
    };
    for (const SingularGolden& item : cases) {
        const Quatd source = item.source.ToQuaternion();
        auto recovered = Rotator<double>::TryFromQuaternion(source);
        REQUIRE(recovered);
        CHECK(Near(recovered.Value().yaw, item.expected.yaw, 1.0e-9));
        if (item.singular) {
            CHECK(recovered.Value().pitch == item.expected.pitch);
            CHECK(recovered.Value().roll == 0.0);
        } else {
            CHECK(Near(recovered.Value().pitch, item.expected.pitch, 1.0e-9));
            CHECK(Near(recovered.Value().roll, item.expected.roll, 1.0e-9));
        }
    }

    // 정확히 ±90에서는 같은 회전이고, 임계 안쪽(89.99)은 UE와 같이 roll만큼 달라진다.
    for (int index = 0; index < 2; ++index) {
        const Quatd source = cases[index].source.ToQuaternion();
        CHECK(AngularDistance(Rotator<double>::TryFromQuaternion(source).Value().ToQuaternion(),
                              source) < 1.0e-12);
    }

    const Quatd nearPole = cases[2].source.ToQuaternion();
    CHECK(AngularDistance(Rotator<double>::TryFromQuaternion(nearPole).Value().ToQuaternion(),
                          nearPole) > 1.0e-4);
}

TEST(Rotator_RandomRoundTripAndAxisAngleDecomposition) {
    MathRandom random(0x0f1e2d3c4b5a6978ull ^ CurrentSeed());
    for (int iteration = 0; iteration < 5000; ++iteration) {
        // |pitch| <= 89이면 s <= sin(89°)/2 = 0.49992로 특이점 임계 밖이다.
        const Rotator<double> source(random.Range(-89.0, 89.0), random.Range(-180.0, 180.0),
                                     random.Range(-180.0, 180.0));
        const Quatd q = source.ToQuaternion();
        // UE 단일식 = Q(+Z, yaw) * Q(+Y, -pitch) * Q(+X, -roll).
        // 반각 cos이 모두 양수라 부호도 같다.
        CHECK(q.Equals(ComposeAxisAngles(source), 1.0e-15));
        CHECK(source.Vector().Equals(q.GetForwardVector(), 1.0e-14));
        auto recovered = Rotator<double>::TryFromQuaternion(q);
        REQUIRE(recovered);
        CHECK(recovered.Value().Equals(source, 1.0e-9));
        CHECK(AngularDistance(recovered.Value().ToQuaternion(), q) < 1.0e-12);
        // q와 -q는 같은 각이다.
        auto negated = Rotator<double>::TryFromQuaternion(Quatd(-q.x, -q.y, -q.z, -q.w));
        REQUIRE(negated);
        CHECK(negated.Value().Equals(recovered.Value(), 1.0e-12));
    }

    // float: FastAsin(2.3e-7 rad)과 float 반올림을 포함해도 같은 회전이다.
    for (int iteration = 0; iteration < 2000; ++iteration) {
        const Rotator<float> source(static_cast<float>(random.Range(-80.0, 80.0)),
                                    static_cast<float>(random.Range(-180.0, 180.0)),
                                    static_cast<float>(random.Range(-180.0, 180.0)));
        auto recovered = Rotator<float>::TryFromQuaternion(source.ToQuaternion());
        REQUIRE(recovered);
        CHECK(AngularDistance(Quatd(recovered.Value().ToQuaternion()),
                              Quatd(source.ToQuaternion())) < 2.0e-6);
    }
}

TEST(Rotator_ClampAndNormalizeAxisMatchUnreal) {
    // UE TRotator::ClampAxis·NormalizeAxis(Rotator.h:688-716) probe 값과 정확히 같다.
    // unwound는 UE FMath::UnwindDegrees(±360 루프)의 값이다. NormalizeAxis와 달리 -180 쪽 경계를
    // 남긴다(-180, -540).
    struct AxisCase {
        double input;
        double clamped;
        double normalized;
        double unwound;
    };

    const AxisCase cases[] = {{0.0, 0.0, 0.0, 0.0},           {180.0, 180.0, 180.0, 180.0},
                              {-180.0, 180.0, 180.0, -180.0}, {190.0, 190.0, -170.0, -170.0},
                              {-190.0, 170.0, 170.0, 170.0},  {540.0, 180.0, 180.0, 180.0},
                              {-540.0, 180.0, 180.0, -180.0}, {360.0, 0.0, 0.0, 0.0},
                              {-360.0, 0.0, 0.0, 0.0},        {725.0, 5.0, 5.0, 5.0},
                              {-90.0, 270.0, -90.0, -90.0},   {359.5, 359.5, -0.5, -0.5}};
    for (const AxisCase& item : cases) {
        CHECK(Rotator<double>::ClampAxis(item.input) == item.clamped);
        CHECK(Rotator<double>::NormalizeAxis(item.input) == item.normalized);
        CHECK(math::UnwindDegrees(item.input) == item.unwound);
        const float single = static_cast<float>(item.input);
        CHECK(Rotator<float>::ClampAxis(single) == static_cast<float>(item.clamped));
        CHECK(Rotator<float>::NormalizeAxis(single) == static_cast<float>(item.normalized));
    }

    // 의도적 차이: UE ClampAxis(-1e-20)은 반올림으로 360(선언 구간 [0, 360) 밖)이고 Rocks는
    // 0이다. NormalizeAxis는 둘 다 0이다.
    CHECK(Rotator<double>::ClampAxis(-1.0e-20) == 0.0);
    CHECK(Rotator<double>::NormalizeAxis(-1.0e-20) == 0.0);
    CHECK(Rotator<float>::ClampAxis(-1.0e-20f) == 0.0f);
    CHECK(math::IsNaN(Rotator<double>::ClampAxis(platform::PlatformMath::Infinity<double>())));
    // 성분별 적용과 회전 보존.
    const Rotator<double> wound(-190.0, 540.0, 725.0);
    CHECK(wound.GetNormalized() == Rotator<double>(170.0, 180.0, 5.0));
    CHECK(wound.GetDenormalized() == Rotator<double>(170.0, 180.0, 5.0));
    CHECK(Rotator<double>(-90.0, -1.0, 0.0).GetDenormalized() ==
          Rotator<double>(270.0, 359.0, 0.0));
    CHECK(AngularDistance(wound.GetNormalized().ToQuaternion(), wound.ToQuaternion()) < 1.0e-14);
    // Equals는 360° 감김을 같게 보고, 다른 성분 조합의 같은 회전은 구분한다.
    CHECK(Rotator<double>(0.0, 360.0, -720.0).Equals(Rotator<double>(), 0.0));
    CHECK(!Rotator<double>(180.0, 0.0, 0.0).Equals(Rotator<double>(0.0, 180.0, 180.0), 1.0e-6));
    CHECK(AngularDistance(Rotator<double>(180.0, 0.0, 0.0).ToQuaternion(),
                          Rotator<double>(0.0, 180.0, 180.0).ToQuaternion()) < 1.0e-15);
    const double nan = platform::PlatformMath::QuietNaN<double>();
    CHECK(!Rotator<double>(nan, 0.0, 0.0).IsFinite());
    CHECK(!Rotator<double>(nan, 0.0, 0.0).Equals(Rotator<double>(nan, 0.0, 0.0), 1.0));
}

TEST(MathInterp_SmoothStepAndCubic) {
    CHECK(math::SmoothStep(0.0, 1.0, -1.0) == 0.0);
    CHECK(math::SmoothStep(0.0, 1.0, 2.0) == 1.0);
    CHECK(math::SmoothStep(0.0, 1.0, 0.5) == 0.5);
    CHECK(math::SmoothStep(2.0f, 4.0f, 3.0f) == 0.5f);
    CHECK(math::IsNaN(math::SmoothStep(
        0.0, 1.0, static_cast<double>(platform::PlatformMath::QuietNaN<double>()))));
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
    CHECK(math::IsNaN(
        math::Wrap(static_cast<double>(platform::PlatformMath::Infinity<double>()), 0.0, 1.0)));
    CHECK(Near(math::FindDeltaAngleRadians(3.0, -3.0), 2.0 * math::Pi<double> - 6.0, 1.0e-12));
    CHECK(Near(math::FindDeltaAngleRadians(-3.0, 3.0), 6.0 - 2.0 * math::Pi<double>, 1.0e-12));
}

TEST(Interp_PropagatesNaNInsteadOfReturningTarget) {
    const double nan = platform::PlatformMath::QuietNaN<double>();
    CHECK(math::IsNaN(math::InterpTo(0.0, 1.0, 0.1, nan)));
    CHECK(math::IsNaN(math::InterpTo(nan, 1.0, 0.1, 100.0)));
    CHECK(math::IsNaN(math::InterpTo(0.0, nan, 0.1, 0.0)));
    CHECK(math::IsNaN(math::InterpConstantTo(0.0, nan, 1.0, 1.0)));
    CHECK(math::IsNaN(math::InterpConstantTo(0.0, -nan, 1.0, 1.0)));
    CHECK(math::IsNaN(math::InterpConstantTo(nan, 1.0, 1.0, 1.0)));
    // 같은 무한대는 이미 도달한 상태다(차가 NaN이어도 NaN으로 바꾸지 않는다).
    const double infinity = platform::PlatformMath::Infinity<double>();
    CHECK(math::InterpConstantTo(infinity, infinity, 1.0, 1.0) == infinity);
    CHECK(math::InterpConstantTo(-infinity, -infinity, 1.0, 1.0) == -infinity);
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
} // namespace math_rotator_interp_tests_file

} // namespace rocks::test
