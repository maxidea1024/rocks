#include "Tests/Support/Test.h"
#include "Tests/Support/MathTestSupport.h"
#include <Core/Math/MatrixFactory.h>
#include <Core/Math/Transform.h>

namespace rocks::test {
namespace {

Quatd RandomRotation(MathRandom& random) {
    for (;;) {
        const Quatd candidate{random.Range(-1.0, 1.0), random.Range(-1.0, 1.0),
                              random.Range(-1.0, 1.0), random.Range(-1.0, 1.0)};
        if (candidate.LengthSquared() > 0.01 && candidate.LengthSquared() <= 1.0) {
            return candidate.TryNormalize().Value();
        }
    }
}

Vec3d RandomVector(MathRandom& random, double range) {
    return Vec3d{random.Range(-range, range), random.Range(-range, range),
                 random.Range(-range, range)};
}

} // namespace

TEST(Quaternion_AxisAngleFollowsRightHandRule) {
    const Quatd yaw = Quatd::FromUnitAxisAngle(Vec3d::Up(), math::HalfPi<double>);
    CHECK(yaw.RotateVector(Vec3d::Forward()).Equals(Vec3d::Left(), 1.0e-15));
    CHECK(yaw.GetForward().Equals(Vec3d::Left(), 1.0e-15));
    CHECK(yaw.GetUp().Equals(Vec3d::Up(), 1.0e-15));
    CHECK(yaw.UnrotateVector(Vec3d::Left()).Equals(Vec3d::Forward(), 1.0e-15));
    auto created = Quat::TryFromAxisAngle(Vec3(0.0f, 0.0f, 5.0f), math::HalfPi<float>);
    REQUIRE(created);
    CHECK(created.Value().IsNormalized(1.0e-6f));
    CHECK(!Quat::TryFromAxisAngle(Vec3::Zero(), 1.0f));
    CHECK(!Quat::TryFromAxisAngle(Vec3::Up(), __builtin_nanf("")));
    CHECK(Quatd() == Quatd::Identity() && Quatd().w == 1.0);
}

TEST(Quaternion_CompositionAppliesRightOperandFirst) {
    const Quatd yaw = Quatd::FromUnitAxisAngle(Vec3d::Up(), math::HalfPi<double>);
    const Quatd roll = Quatd::FromUnitAxisAngle(Vec3d::Forward(), math::HalfPi<double>);
    // (yaw * roll): roll 먼저. Left --roll--> Up --yaw--> Up
    CHECK((yaw * roll).RotateVector(Vec3d::Left()).Equals(Vec3d::Up(), 1.0e-15));
    // (roll * yaw): yaw 먼저. Left --yaw--> Backward --roll--> Backward
    CHECK((roll * yaw).RotateVector(Vec3d::Left()).Equals(Vec3d::Backward(), 1.0e-15));
    // 행렬 합성과 같은 순서다.
    CHECK((yaw * roll).ToMatrix().Equals(yaw.ToMatrix() * roll.ToMatrix(), 1.0e-15));
}

TEST(Quaternion_RandomizedConsistency) {
    MathRandom random(0xa5a5a5a5deadbeefull ^ CurrentSeed());
    for (int iteration = 0; iteration < 5000; ++iteration) {
        const Quatd a = RandomRotation(random);
        const Quatd b = RandomRotation(random);
        const Vec3d v = RandomVector(random, 10.0);
        // 회전은 길이를 보존하고 행렬 경로와 같다.
        CHECK(Near(a.RotateVector(v).Length(), v.Length(), 1.0e-12, 1.0e-13));
        CHECK(a.RotateVector(v).Equals(a.ToMatrix().TransformVector(v), 1.0e-12));
        CHECK((a * b).RotateVector(v).Equals(a.RotateVector(b.RotateVector(v)), 1.0e-12));
        CHECK(a.UnrotateVector(a.RotateVector(v)).Equals(v, 1.0e-12));
        // 행렬 왕복: q와 -q 중 w >= 0인 쪽으로 돌아온다.
        auto recovered = Quatd::TryFromRotationMatrix(a.ToMatrix());
        REQUIRE(recovered);
        CHECK(AngularDistance(recovered.Value(), a) < 1.0e-7);
        CHECK(recovered.Value().w >= 0.0);
        auto inverse = a.TryInverse();
        REQUIRE(inverse);
        CHECK(AngularDistance(a * inverse.Value(), Quatd::Identity()) < 1.0e-7);
        // float SIMD 곱은 double 기준과 가깝다.
        const Quat af(a);
        const Quat bf(b);
        const Quatd productReference = Quatd(af) * Quatd(bf);
        CHECK(Quatd(af * bf).Equals(productReference, 2.0e-7));
    }
}

TEST(Quaternion_FromRotationMatrixCoversAllBranches) {
    // 대각합 음수 분기(각 축 180도 근처)를 모두 거친다.
    const Vec3d axes[] = {Vec3d::UnitX(), Vec3d::UnitY(), Vec3d::UnitZ(),
                          Vec3d(1.0, 1.0, 0.0).TryNormalize().Value()};
    const double angles[] = {math::Pi<double>, math::Pi<double> - 1.0e-6, 2.5, 0.0};
    for (const Vec3d& axis : axes) {
        for (double angle : angles) {
            const Quatd q = Quatd::FromUnitAxisAngle(axis, angle);
            auto recovered = Quatd::TryFromRotationMatrix(q.ToMatrix());
            REQUIRE(recovered);
            CHECK(AngularDistance(recovered.Value(), q) < 1.0e-6);
        }
    }
    Mat4d bad;
    bad.m[0] = __builtin_nan("");
    CHECK(!Quatd::TryFromRotationMatrix(bad));
}

TEST(Quaternion_NormalizeInverseAndAxisAngle) {
    CHECK(!Quat(0.0f, 0.0f, 0.0f, 0.0f).TryNormalize());
    CHECK(!Quat(__builtin_nanf(""), 0.0f, 0.0f, 1.0f).TryNormalize());
    auto big = Quat(0.0f, 0.0f, 3.0e30f, 4.0e30f).TryNormalize();
    REQUIRE(big);
    CHECK(big.Value().Equals(Quat(0.0f, 0.0f, 0.6f, 0.8f), 1.0e-6f));
    CHECK(Quatd(1.0, 2.0, 3.0, 4.0).Conjugate() == Quatd(-1.0, -2.0, -3.0, 4.0));
    auto inverse = Quatd(0.0, 0.0, 0.0, 2.0).TryInverse();
    REQUIRE(inverse);
    CHECK(inverse.Value() == Quatd(0.0, 0.0, 0.0, 0.5));
    CHECK(!Quatd(0.0, 0.0, 0.0, 0.0).TryInverse());
    Vec3d axis;
    double angle = 0.0;
    Quatd::FromUnitAxisAngle(Vec3d::Left(), 1.25).ToAxisAngle(axis, angle);
    CHECK(axis.Equals(Vec3d::Left(), 1.0e-15) && Near(angle, 1.25, 1.0e-15));
    // q와 -q는 같은 축·각을 낸다.
    const Quatd q = Quatd::FromUnitAxisAngle(Vec3d::Up(), 0.5);
    Quatd(-q.x, -q.y, -q.z, -q.w).ToAxisAngle(axis, angle);
    CHECK(axis.Equals(Vec3d::Up(), 1.0e-15) && Near(angle, 0.5, 1.0e-15));
    Quatd::Identity().ToAxisAngle(axis, angle);
    CHECK(axis == Vec3d::UnitX() && angle == 0.0);
}

TEST(Quaternion_SlerpEndpointsAndShortestPath) {
    const Quatd a = Quatd::FromUnitAxisAngle(Vec3d::Up(), 0.2);
    const Quatd b = Quatd::FromUnitAxisAngle(Vec3d::Up(), 1.4);
    CHECK(AngularDistance(Slerp(a, b, 0.0), a) < 1.0e-12);
    CHECK(AngularDistance(Slerp(a, b, 1.0), b) < 1.0e-7);
    const Quatd middle = Slerp(a, b, 0.5);
    CHECK(AngularDistance(middle, Quatd::FromUnitAxisAngle(Vec3d::Up(), 0.8)) < 1.0e-7);
    CHECK(middle.IsNormalized(1.0e-14));
    // -b로 같은 회전을 줘도 최단 경로를 택한다.
    const Quatd negated(-b.x, -b.y, -b.z, -b.w);
    CHECK(AngularDistance(Slerp(a, negated, 0.5), middle) < 1.0e-7);
    CHECK(AngularDistance(a, Quatd(-a.x, -a.y, -a.z, -a.w)) < 1.0e-7);
    // 거의 같은 회전에서는 정규화 선형 보간으로 안정적으로 계산한다.
    const Quatd near = Quatd::FromUnitAxisAngle(Vec3d::Up(), 0.2 + 1.0e-9);
    CHECK(Slerp(a, near, 0.5).IsFinite());
}

TEST(Transform_PointOrderIsScaleRotateTranslate) {
    const Transform transform(Vec3d(10.0, 0.0, 0.0),
                              Quat::FromUnitAxisAngle(Vec3::Up(), math::HalfPi<float>),
                              Vec3(2.0f, 1.0f, 1.0f));
    CHECK(transform.IsValid());
    CHECK(transform.TransformPoint(Vec3d(1.0, 0.0, 0.0)).Equals(Vec3d(10.0, 2.0, 0.0), 1.0e-6));
    CHECK(transform.TransformVector(Vec3d(1.0, 0.0, 0.0)).Equals(Vec3d(0.0, 2.0, 0.0), 1.0e-6));
    CHECK(transform.TransformDirection(Vec3d(1.0, 0.0, 0.0)).Equals(Vec3d(0.0, 1.0, 0.0), 1.0e-6));
    // 행렬 경로와 같은 결과다.
    const Vec3d p(0.3, -2.0, 7.0);
    CHECK(transform.TransformPoint(p).Equals(transform.ToMatrix().TransformPoint(p), 1.0e-12));
    auto back = transform.TryInverseTransformPoint(transform.TransformPoint(p));
    REQUIRE(back);
    CHECK(back.Value().Equals(p, 1.0e-12));
    CHECK(Transform::Identity().TransformPoint(p) == p);
}

TEST(Transform_LargeWorldTranslationKeepsDoublePrecision) {
    // 10^9 m 떨어진 위치에서도 1mm 변위를 잃지 않는다(float라면 64m 단위로 양자화된다).
    const Transform far(Vec3d(1.0e9, -1.0e9, 5.0), Quat::Identity(), Vec3::One());
    const Vec3d moved = far.TransformPoint(Vec3d(0.001, 0.0, 0.0));
    CHECK(Near(moved.x - 1.0e9, 0.001, 1.0e-6));
    auto local = far.TryInverseTransformPoint(Vec3d(1.0e9 + 0.25, -1.0e9, 5.0));
    REQUIRE(local);
    CHECK(local.Value().Equals(Vec3d(0.25, 0.0, 0.0), 1.0e-6));
}

TEST(Transform_InverseFailsOnZeroScale) {
    const Transform flat(Vec3d::Zero(), Quat::Identity(), Vec3(1.0f, 0.0f, 1.0f));
    CHECK(flat.TryInverseTransformPoint(Vec3d::One()).Error() == ErrorCode::InvalidArgument);
    CHECK(!flat.TryInverseTransformVector(Vec3d::One()));
    CHECK(flat.TryInverse().Error() == ErrorCode::InvalidArgument);
    const Transform invalid(Vec3d(__builtin_nan(""), 0.0, 0.0), Quat::Identity(), Vec3::One());
    CHECK(!invalid.IsValid());
    CHECK(!Transform(Vec3d::Zero(), Quat(0.0f, 0.0f, 0.0f, 2.0f), Vec3::One()).IsValid());
}

TEST(Transform_ComposeMatchesAffineProductWhenRepresentable) {
    MathRandom random(0x13579bdf2468ace0ull ^ CurrentSeed());
    for (int iteration = 0; iteration < 2000; ++iteration) {
        const float parentScale = static_cast<float>(random.Range(0.25, 4.0));
        const Transform parent(RandomVector(random, 100.0), Quat(RandomRotation(random)),
                               Vec3(parentScale, parentScale, parentScale));
        const Transform local{RandomVector(random, 10.0), Quat(RandomRotation(random)),
                              Vec3{static_cast<float>(random.Range(0.5, 2.0)),
                                   static_cast<float>(random.Range(0.5, 2.0)),
                                   static_cast<float>(random.Range(0.5, 2.0))}};
        auto composed = Transform::TryCompose(parent, local);
        REQUIRE(composed);
        const Vec3d p = RandomVector(random, 5.0);
        const Vec3d expected = parent.TransformPoint(local.TransformPoint(p));
        CHECK(composed.Value().TransformPoint(p).Equals(expected, 1.0e-4));
        // 균일 scale·회전 변환은 TRS 역변환이 존재한다.
        auto inverse = parent.TryInverse();
        REQUIRE(inverse);
        CHECK(inverse.Value().TransformPoint(parent.TransformPoint(p)).Equals(p, 1.0e-4));
    }
}

TEST(Transform_ShearIsRejectedInsteadOfApproximated) {
    // 비균일 parent scale + 45도 child 회전 = shear. TRS로 근사하지 않고 Unsupported다.
    const Transform parent(Vec3d::Zero(), Quat::Identity(), Vec3(1.0f, 3.0f, 1.0f));
    const Transform child(Vec3d::Zero(), Quat::FromUnitAxisAngle(Vec3::Up(), 0.785398163f),
                          Vec3::One());
    auto composed = Transform::TryCompose(parent, child);
    CHECK(!composed && composed.Error() == ErrorCode::Unsupported);
    // 정확한 affine 결과는 행렬로 얻을 수 있다.
    const Mat4d affine = parent.ToMatrix() * child.ToMatrix();
    const Vec3d p(1.0, 0.0, 0.0);
    CHECK(affine.TransformPoint(p).Equals(parent.TransformPoint(child.TransformPoint(p)), 1.0e-12));
    // 비균일 scale + 회전의 역변환도 TRS가 아니다.
    const Transform skewed(Vec3d::Zero(), Quat::FromUnitAxisAngle(Vec3::Up(), 0.5f),
                           Vec3(1.0f, 2.0f, 1.0f));
    CHECK(skewed.TryInverse().Error() == ErrorCode::Unsupported);
}

TEST(Transform_FromMatrixHandlesMirrorAndRejectsProjective) {
    const Transform mirrored(Vec3d(1.0, 2.0, 3.0), Quat::FromUnitAxisAngle(Vec3::Up(), 0.4f),
                             Vec3(-2.0f, 1.0f, 3.0f));
    auto decomposed = Transform::TryFromMatrix(mirrored.ToMatrix());
    REQUIRE(decomposed);
    const Vec3d p(0.5, -1.5, 2.5);
    CHECK(decomposed.Value().TransformPoint(p).Equals(mirrored.TransformPoint(p), 1.0e-5));
    CHECK(decomposed.Value().scale.x < 0.0f); // 반사는 x scale 부호로 표현한다.
    Mat4d projective = Mat4d::Identity();
    projective.At(3, 0) = 0.1;
    CHECK(Transform::TryFromMatrix(projective).Error() == ErrorCode::InvalidArgument);
    CHECK(!Transform::TryFromMatrix(Mat4d::Scale(Vec3d(1.0, 0.0, 1.0))));
}

TEST(Quaternion_AngularDistanceResolvesSmallAngles) {
    // 리뷰 재현 입력: 2·acos 방식은 같은 회전에서 9.77e-4 rad를 냈다.
    const Quat q(-0x1.9cee62p-1f, 0x1.448548p-2f, 0x1.cf8c7ap-2f, -0x1.ae845ap-3f);
    REQUIRE(q.IsNormalized(1.0e-6f));
    CHECK(AngularDistance(q, q) == 0.0f);
    CHECK(AngularDistance(q, Quat(-q.x, -q.y, -q.z, -q.w)) == 0.0f);
    // 작은 각 분해능: double 1e-9 rad, float 1e-4 rad.
    const Quatd base = Quatd::FromUnitAxisAngle(Vec3d(0.0, 0.6, 0.8), 0.7);
    const Quatd nudged = base * Quatd::FromUnitAxisAngle(Vec3d::UnitX(), 1.0e-9);
    CHECK(Near(AngularDistance(base, nudged), 1.0e-9, 1.0e-12));
    const Quat baseFloat = Quat::FromUnitAxisAngle(Vec3(0.0f, 0.6f, 0.8f), 0.7f);
    const Quat nudgedFloat = baseFloat * Quat::FromUnitAxisAngle(Vec3::UnitX(), 1.0e-4f);
    CHECK(Near(static_cast<double>(AngularDistance(baseFloat, nudgedFloat)), 1.0e-4, 2.0e-6));
    // 큰 각과 반구 경계.
    CHECK(Near(AngularDistance(Quatd(), Quatd::FromUnitAxisAngle(Vec3d::UnitZ(), 3.0)), 3.0,
               1.0e-12));
    CHECK(Near(AngularDistance(Quatd(), Quatd(0.0, 0.0, 1.0, 0.0)), math::Pi<double>, 1.0e-12));
    CHECK(Near(AngularDistance(Quatd(), Quatd::FromUnitAxisAngle(Vec3d::UnitZ(), 4.0)),
               2.0 * math::Pi<double> - 4.0, 1.0e-12));
}

TEST(Quaternion_TryInverseHandlesExtremeMagnitudes) {
    // 제곱 길이가 underflow/overflow해도 역원이 표현 가능하면 성공한다.
    auto tiny = Quat(1.0e-30f, 0.0f, 0.0f, 0.0f).TryInverse();
    REQUIRE(tiny);
    CHECK(Near(static_cast<double>(tiny.Value().x), -1.0e30, 1.0e24));
    auto huge = Quat(0.0f, 0.0f, 0.0f, 1.0e20f).TryInverse();
    REQUIRE(huge);
    CHECK(Near(static_cast<double>(huge.Value().w), 1.0e-20, 1.0e-26));
    // 일반 값: q * q⁻¹ = 항등.
    const Quatd general(0.3, -1.2, 2.5, 0.7);
    auto inverse = general.TryInverse();
    REQUIRE(inverse);
    CHECK((general * inverse.Value()).Equals(Quatd(), 1.0e-15));
    // 실패: 영, 비유한, 역원이 float 범위를 넘음(1/1e-40 > FLT_MAX).
    CHECK(Quat(0.0f, 0.0f, 0.0f, 0.0f).TryInverse().Error() == ErrorCode::InvalidArgument);
    CHECK(!Quat(__builtin_nanf(""), 0.0f, 0.0f, 1.0f).TryInverse());
    CHECK(!Quat(1.0e-40f, 0.0f, 0.0f, 0.0f).TryInverse());
}

TEST(Quaternion_SlerpPropagatesNonFiniteInput) {
    const Quat a = Quat::FromUnitAxisAngle(Vec3::UnitZ(), 0.3f);
    const Quat bad(__builtin_nanf(""), 0.0f, 0.0f, 1.0f);
    CHECK(!Slerp(a, bad, 0.5f).IsFinite());
    CHECK(!Slerp(bad, a, 0.5f).IsFinite());
    CHECK(!Slerp(a, Quat::FromUnitAxisAngle(Vec3::UnitX(), 0.2f), __builtin_nanf("")).IsFinite());
    // 유한 입력은 그대로 정규화된 결과다.
    CHECK(Slerp(a, a, 0.5f).IsNormalized(1.0e-6f));
}

TEST(Quaternion_AngularDistanceStaysWithinPi) {
    // 재검토 재현 입력: atan2 식이 pi를 1 ulp 넘었다.
    const Quat a(-0x1.2723a6p-1f, -0x1.81c81ap-5f, -0x1.65e428p-1f, 0x1.aeabb8p-2f);
    const Quat b(-0x1.3f7cecp-1f, 0x1.b126fcp-2f, 0x1.3cdc2ep-1f, 0x1.c3f1f4p-3f);
    CHECK(AngularDistance(a, b) <= math::Pi<float>);
    MathRandom random(0xa11a5ull ^ CurrentSeed());
    for (int index = 0; index < 20000; ++index) {
        auto first = Quatd{random.Range(-1.0, 1.0), random.Range(-1.0, 1.0),
                           random.Range(-1.0, 1.0), random.Range(-1.0, 1.0)}
                         .TryNormalize();
        auto second = Quatd{random.Range(-1.0, 1.0), random.Range(-1.0, 1.0),
                            random.Range(-1.0, 1.0), random.Range(-1.0, 1.0)}
                          .TryNormalize();
        if (!first || !second) {
            continue;
        }
        // 거의 직교(각 ≈ pi)인 쌍을 만든다.
        const Quatd p = first.Value();
        const double along = Dot(p, second.Value());
        auto orthogonal = Quatd(second.Value().x - p.x * along, second.Value().y - p.y * along,
                                second.Value().z - p.z * along, second.Value().w - p.w * along)
                              .TryNormalize();
        if (!orthogonal) {
            continue;
        }
        const double distance = AngularDistance(p, orthogonal.Value());
        CHECK(distance >= 0.0 && distance <= math::Pi<double>);
        const float single = AngularDistance(Quat(p), Quat(orthogonal.Value()));
        CHECK(single >= 0.0f && single <= math::Pi<float>);
    }
    CHECK(math::IsNaN(AngularDistance(Quat(__builtin_nanf(""), 0.0f, 0.0f, 1.0f), a)));
}

} // namespace rocks::test
