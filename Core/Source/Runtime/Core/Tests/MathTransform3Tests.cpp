#include <TestSupport/Test.h>
#include <TestSupport/MathTestSupport.h>
#include <Core/Math/Transform.h>
#include <Platform/Memory.h>
#include <Platform/PlatformMath.h>

// Transform3f·Transform3d(정밀도가 하나인 TRS)와 세 Transform 사이 변환 시험이다
// (Docs/Design/ReflectionExtensions.ko.md 3.2·17절 B9b). 혼합 정밀도 Transform 자체의 시험은
// MathQuaternionTransformTests.cpp에 있다.
namespace rocks::test {
namespace {

template <typename T>
constexpr T PrecisionTolerance(double singleValue, double doubleValue) noexcept {
    return static_cast<T>(IsSameV<T, float> ? singleValue : doubleValue);
}

Quatd RandomUnitQuaternion(MathRandom& random) {
    for (;;) {
        const Quatd candidate{random.Range(-1.0, 1.0), random.Range(-1.0, 1.0),
                              random.Range(-1.0, 1.0), random.Range(-1.0, 1.0)};
        if (candidate.LengthSquared() > 0.01 && candidate.LengthSquared() <= 1.0) {
            return candidate.TryNormalize().Value();
        }
    }
}

template <typename T>
Quaternion<T> RandomRotation(MathRandom& random) {
    return Quaternion<T>(RandomUnitQuaternion(random));
}

template <typename T>
Vector3<T> RandomVector(MathRandom& random, double range) {
    return Vector3<T>{static_cast<T>(random.Range(-range, range)),
                      static_cast<T>(random.Range(-range, range)),
                      static_cast<T>(random.Range(-range, range))};
}

// 크기 [0.25, 4)의 축별 scale이다. allowNegative면 성분마다 1/4 확률로 음수다.
template <typename T>
Vector3<T> RandomScale(MathRandom& random, bool allowNegative) {
    auto component = [&random, allowNegative]() {
        const double magnitude = random.Range(0.25, 4.0);
        const bool negative = allowNegative && random.Unit() < 0.25;
        return static_cast<T>(negative ? -magnitude : magnitude);
    };

    return Vector3<T>{component(), component(), component()};
}

template <typename T>
bool NearVector(const Vector3<T>& a, const Vector3<T>& b, T absolute) noexcept {
    return Near(a.x, b.x, absolute) && Near(a.y, b.y, absolute) && Near(a.z, b.z, absolute);
}

// 세 Transform 모두 성분 이름이 같으므로 정확한 성분 비교를 하나로 쓴다.
template <typename TransformType>
bool SameComponents(const TransformType& a, const TransformType& b) noexcept {
    return a.translation == b.translation && a.rotation == b.rotation && a.scale == b.scale;
}

Mat4d Widen(const Mat4& matrix) noexcept {
    Mat4d result;
    for (int index = 0; index < 16; ++index) {
        result.m[index] = static_cast<double>(matrix.m[index]);
    }

    return result;
}

// 유한한 float 값의 +Inf 쪽 이웃이다.
float NextFloatUp(float value) noexcept {
    if (value == 0.0f) {
        return platform::BitCast<float>(1u);
    }

    const uint32 bits = platform::BitCast<uint32>(value);
    return platform::BitCast<float>(value > 0.0f ? bits + 1u : bits - 1u);
}

float NextFloatDown(float value) noexcept {
    return -NextFloatUp(-value);
}

// rounded가 exact에 가장 가까운 float이고 같은 거리면 짝수 가수인지 본다(IEEE 기본 반올림).
// 이웃과의 차이는 서로 가까운 double끼리의 뺄셈이라 정확하다.
bool IsNearestEvenFloat(double exact, float rounded) noexcept {
    const double here = math::Abs(exact - static_cast<double>(rounded));
    const double up = math::Abs(exact - static_cast<double>(NextFloatUp(rounded)));
    const double down = math::Abs(exact - static_cast<double>(NextFloatDown(rounded)));
    if (here > up || here > down) {
        return false;
    }

    if (here == up || here == down) {
        return (platform::BitCast<uint32>(rounded) & 1u) == 0u;
    }

    return true;
}

bool IsNearestEvenVector(const Vec3d& exact, const Vec3& rounded) noexcept {
    return IsNearestEvenFloat(exact.x, rounded.x) && IsNearestEvenFloat(exact.y, rounded.y) &&
           IsNearestEvenFloat(exact.z, rounded.z);
}

bool IsNearestEvenQuaternion(const Quatd& exact, const Quat& rounded) noexcept {
    return IsNearestEvenFloat(exact.x, rounded.x) && IsNearestEvenFloat(exact.y, rounded.y) &&
           IsNearestEvenFloat(exact.z, rounded.z) && IsNearestEvenFloat(exact.w, rounded.w);
}

template <typename T>
void CheckPointOrderAndIdentity() {
    using V = Vector3<T>;
    using Q = Quaternion<T>;
    const T tolerance = PrecisionTolerance<T>(1.0e-5, 1.0e-12);
    const Transform3<T> identity;
    CHECK(identity.IsValid());
    CHECK(identity.translation == V::Zero() && identity.rotation == Q::Identity() &&
          identity.scale == V::One());
    CHECK(SameComponents(identity, Transform3<T>::Identity()));
    CHECK(identity.ToMatrix() == Matrix4<T>::Identity());
    const V p(static_cast<T>(0.3), static_cast<T>(-2), static_cast<T>(7));
    CHECK(identity.TransformPoint(p) == p);

    // scale → rotation → translation: x축 점은 2배 뒤 +Z 90도 회전으로 +Y, 이동으로 x += 10.
    const Transform3<T> transform(V(static_cast<T>(10), 0, 0),
                                  Q::FromUnitAxisAngle(V::Up(), math::HalfPi<T>),
                                  V(static_cast<T>(2), static_cast<T>(1), static_cast<T>(1)));
    CHECK(transform.IsValid());
    const V unitX = V::UnitX();
    CHECK(transform.TransformPoint(unitX).Equals(V(static_cast<T>(10), static_cast<T>(2), 0),
                                                 tolerance));
    CHECK(transform.TransformVector(unitX).Equals(V(0, static_cast<T>(2), 0), tolerance));
    CHECK(transform.TransformDirection(unitX).Equals(V::UnitY(), tolerance));
    CHECK(transform.TransformPoint(p).Equals(transform.ToMatrix().TransformPoint(p), tolerance));
    CHECK(transform.TransformVector(p).Equals(transform.ToMatrix().TransformVector(p), tolerance));
    auto back = transform.TryInverseTransformPoint(transform.TransformPoint(p));
    REQUIRE(back);
    CHECK(back.Value().Equals(p, tolerance));
    auto vectorBack = transform.TryInverseTransformVector(transform.TransformVector(p));
    REQUIRE(vectorBack);
    CHECK(vectorBack.Value().Equals(p, tolerance));

    CHECK(!transform.HasUniformScale(static_cast<T>(0.5)));
    CHECK(Transform3<T>(V::Zero(), Q::Identity(), V(-2, 2, 2)).HasUniformScale(0));

    // 길이 오차가 IsValid 허용치 안인 rotation은 정규화한 회전으로 쓴다(정규화하지 않으면 결과가
    // |q|² 배로 약 1e-4 상대 오차가 난다).
    const Q unit =
        Q::FromUnitAxisAngle(V(0, static_cast<T>(0.6), static_cast<T>(0.8)), static_cast<T>(0.7));
    const T stretch = static_cast<T>(1.00005);
    const Q stretched(unit.x * stretch, unit.y * stretch, unit.z * stretch, unit.w * stretch);
    const V offset(1, 2, 3);
    const V scale(2, 3, 4);
    const Transform3<T> loose(offset, stretched, scale);
    const Transform3<T> exact(offset, unit, scale);
    CHECK(loose.IsValid());
    const T looseTolerance = PrecisionTolerance<T>(1.0e-4, 1.0e-12);
    CHECK(loose.TransformPoint(p).Equals(exact.TransformPoint(p), looseTolerance));
    CHECK(loose.ToMatrix().Equals(exact.ToMatrix(), looseTolerance));
    // 제곱 길이가 빠른 정규화 범위 밖이면(0 근처·아주 큼, float에서는 제곱이 subnormal·Inf)
    // 성분을 먼저 나누는 경로로 같은 회전을 얻는다. 이런 값은 IsValid가 거짓이지만 계산은 정의된다.
    const T factors[] = {static_cast<T>(1.0e-20), static_cast<T>(1.0e20)};
    for (const T factor : factors) {
        const Transform3<T> far(
            offset, Q(unit.x * factor, unit.y * factor, unit.z * factor, unit.w * factor), scale);
        CHECK(!far.IsValid());
        CHECK(far.TransformPoint(p).Equals(exact.TransformPoint(p), looseTolerance));
    }
}

template <typename T>
void CheckMatrixRoundTrip(uint64 salt) {
    const T matrixTolerance = PrecisionTolerance<T>(1.0e-5, 1.0e-12);
    const T pointTolerance = PrecisionTolerance<T>(2.0e-4, 1.0e-10);
    const double scaleRelative = IsSameV<T, float> ? 2.0e-6 : 1.0e-12;
    const double angleTolerance = IsSameV<T, float> ? 1.0e-5 : 1.0e-9;
    MathRandom random(salt ^ CurrentSeed());
    for (int iteration = 0; iteration < 2000; ++iteration) {
        const Vector3<T> scale = RandomScale<T>(random, true);
        const Transform3<T> original(RandomVector<T>(random, 100.0), RandomRotation<T>(random),
                                     scale);
        auto decomposed = Transform3<T>::TryFromMatrix(original.ToMatrix());
        REQUIRE(decomposed);
        const Transform3<T>& value = decomposed.Value();
        CHECK(value.IsValid());
        // 이동은 행렬 마지막 열을 그대로 읽으므로 정확히 돌아온다.
        CHECK(value.translation == original.translation);
        CHECK(value.ToMatrix().Equals(original.ToMatrix(), matrixTolerance));
        const Vector3<T> p = RandomVector<T>(random, 5.0);
        CHECK(NearVector(value.TransformPoint(p), original.TransformPoint(p), pointTolerance));

        // 크기는 열 길이로 돌아오고 반사(행렬식 음수)는 x 부호 하나로 모인다.
        const int negatives = (scale.x < 0 ? 1 : 0) + (scale.y < 0 ? 1 : 0) + (scale.z < 0 ? 1 : 0);
        const bool reflected = negatives % 2 == 1;
        CHECK((value.scale.x < 0) == reflected && value.scale.y > 0 && value.scale.z > 0);
        CHECK(Near(math::Abs(static_cast<double>(value.scale.x)),
                   math::Abs(static_cast<double>(scale.x)), 0.0, scaleRelative));
        CHECK(Near(static_cast<double>(value.scale.y), math::Abs(static_cast<double>(scale.y)), 0.0,
                   scaleRelative));
        CHECK(Near(static_cast<double>(value.scale.z), math::Abs(static_cast<double>(scale.z)), 0.0,
                   scaleRelative));
        if (scale.x > 0 && scale.y > 0 && scale.z > 0) {
            // 양의 scale이면 회전도 같은 회전으로 돌아온다(q와 -q 중 w >= 0).
            CHECK(AngularDistance(Quatd(value.rotation), Quatd(original.rotation)) <=
                  angleTolerance);
            CHECK(value.rotation.w >= 0);
        }
    }
}

template <typename T>
void CheckComposeAndInverse(uint64 salt) {
    const T tolerance = PrecisionTolerance<T>(2.0e-3, 1.0e-9);
    MathRandom random(salt ^ CurrentSeed());
    for (int iteration = 0; iteration < 2000; ++iteration) {
        const T parentScale = static_cast<T>(random.Range(0.25, 4.0));
        const Transform3<T> parent(RandomVector<T>(random, 100.0), RandomRotation<T>(random),
                                   Vector3<T>::Splat(parentScale));
        const Transform3<T> local(RandomVector<T>(random, 10.0), RandomRotation<T>(random),
                                  RandomScale<T>(random, false));
        auto composed = Transform3<T>::TryCompose(parent, local);
        REQUIRE(composed);
        const Vector3<T> p = RandomVector<T>(random, 5.0);
        const Vector3<T> expected = parent.TransformPoint(local.TransformPoint(p));
        CHECK(NearVector(composed.Value().TransformPoint(p), expected, tolerance));
        // 합성은 ToMatrix 곱의 분해로 정의된다.
        auto viaMatrix = Transform3<T>::TryFromMatrix(parent.ToMatrix() * local.ToMatrix());
        REQUIRE(viaMatrix);
        CHECK(SameComponents(composed.Value(), viaMatrix.Value()));

        // 균일 scale·회전 변환은 TRS 역변환이 있고, 역과의 합성은 항등에 가깝다.
        auto inverse = parent.TryInverse();
        REQUIRE(inverse);
        CHECK(NearVector(inverse.Value().TransformPoint(parent.TransformPoint(p)), p, tolerance));
        auto pointBack = parent.TryInverseTransformPoint(parent.TransformPoint(p));
        REQUIRE(pointBack);
        CHECK(NearVector(pointBack.Value(), p, tolerance));
        auto roundTrip = Transform3<T>::TryCompose(parent, inverse.Value());
        REQUIRE(roundTrip);
        CHECK(NearVector(roundTrip.Value().TransformPoint(p), p, tolerance));
    }
}

template <typename T>
void CheckShearAndZeroScale() {
    using V = Vector3<T>;
    using Q = Quaternion<T>;
    const T tolerance = PrecisionTolerance<T>(1.0e-5, 1.0e-12);
    // 비균일 parent scale + 45도 child 회전 = shear. TRS로 근사하지 않고 Unsupported다.
    const Transform3<T> parent(V::Zero(), Q::Identity(), V(1, 3, 1));
    const Transform3<T> child(V::Zero(), Q::FromUnitAxisAngle(V::Up(), math::Pi<T> / 4), V::One());
    auto composed = Transform3<T>::TryCompose(parent, child);
    CHECK(!composed && composed.Error() == ErrorCode::Unsupported);
    // 정확한 affine 결과는 행렬 곱으로 얻는다.
    const Matrix4<T> affine = parent.ToMatrix() * child.ToMatrix();
    const V unitX = V::UnitX();
    CHECK(affine.TransformPoint(unitX).Equals(parent.TransformPoint(child.TransformPoint(unitX)),
                                              tolerance));
    // 비균일 scale + 회전의 역변환도 TRS가 아니다. 회전이 없으면 비균일 scale도 역변환이 된다.
    const Transform3<T> skewed(V::Zero(), Q::FromUnitAxisAngle(V::Up(), static_cast<T>(0.5)),
                               V(1, 2, 1));
    CHECK(skewed.TryInverse().Error() == ErrorCode::Unsupported);
    const Transform3<T> axisAligned(V(1, 2, 3), Q::Identity(), V(1, 2, 4));
    auto axisInverse = axisAligned.TryInverse();
    REQUIRE(axisInverse);
    const V p(static_cast<T>(0.5), static_cast<T>(-1.5), static_cast<T>(2.5));
    CHECK(axisInverse.Value().TransformPoint(axisAligned.TransformPoint(p)).Equals(p, tolerance));

    // 0 scale: 값은 유효하지만 역변환·분해·합성은 InvalidArgument다.
    const Transform3<T> flat(V::Zero(), Q::Identity(), V(1, 0, 1));
    CHECK(flat.IsValid());
    CHECK(flat.TryInverseTransformPoint(V::One()).Error() == ErrorCode::InvalidArgument);
    CHECK(flat.TryInverseTransformVector(V::One()).Error() == ErrorCode::InvalidArgument);
    CHECK(flat.TryInverse().Error() == ErrorCode::InvalidArgument);
    CHECK(Transform3<T>::TryFromMatrix(flat.ToMatrix()).Error() == ErrorCode::InvalidArgument);
    CHECK(Transform3<T>::TryCompose(flat, axisAligned).Error() == ErrorCode::InvalidArgument);
    // 회전된 child와 합성하면 0 길이 열 대신 평행한 열이 생겨 shear와 같이 Unsupported다(혼합
    // 정밀도 Transform과 같은 판정 순서).
    CHECK(Transform3<T>::TryCompose(flat, child).Error() == ErrorCode::Unsupported);

    // 비유한 성분·비단위 회전·사영 행렬.
    const T nan = platform::PlatformMath::QuietNaN<T>();
    CHECK(!Transform3<T>(V(nan, 0, 0), Q::Identity(), V::One()).IsValid());
    CHECK(!Transform3<T>(V::Zero(), Q(0, 0, 0, 2), V::One()).IsValid());
    const Transform3<T> nanScale(V::Zero(), Q::Identity(), V(1, nan, 1));
    CHECK(!nanScale.IsValid());
    CHECK(nanScale.TryInverse().Error() == ErrorCode::InvalidArgument);
    CHECK(nanScale.TryInverseTransformPoint(V::One()).Error() == ErrorCode::InvalidArgument);
    Matrix4<T> projective;
    projective.At(3, 0) = static_cast<T>(0.1);
    CHECK(Transform3<T>::TryFromMatrix(projective).Error() == ErrorCode::InvalidArgument);
    Matrix4<T> broken;
    broken.m[5] = nan;
    CHECK(Transform3<T>::TryFromMatrix(broken).Error() == ErrorCode::InvalidArgument);

    // 반사는 x scale 부호로 표현하고, 균일 크기 반사는 역변환도 TRS다.
    const Transform3<T> mirrored(V(1, 2, 3), Q::FromUnitAxisAngle(V::Up(), static_cast<T>(0.4)),
                                 V(-2, 1, 3));
    auto decomposed = Transform3<T>::TryFromMatrix(mirrored.ToMatrix());
    REQUIRE(decomposed);
    CHECK(decomposed.Value().scale.x < 0);
    CHECK(decomposed.Value().TransformPoint(p).Equals(mirrored.TransformPoint(p),
                                                      PrecisionTolerance<T>(1.0e-5, 1.0e-12)));
    const Transform3<T> uniformMirror(
        V(1, 2, 3), Q::FromUnitAxisAngle(V::Left(), static_cast<T>(0.9)), V(2, -2, 2));
    auto mirrorInverse = uniformMirror.TryInverse();
    REQUIRE(mirrorInverse);
    CHECK(mirrorInverse.Value().scale.x < 0);
    CHECK(mirrorInverse.Value()
              .TransformPoint(uniformMirror.TransformPoint(p))
              .Equals(p, PrecisionTolerance<T>(1.0e-5, 1.0e-12)));
}

} // namespace

TEST(Transform3_PointOrderIsScaleRotateTranslate) {
    CheckPointOrderAndIdentity<float>();
    CheckPointOrderAndIdentity<double>();
}

TEST(Transform3_MatrixRoundTripKeepsTrs) {
    CheckMatrixRoundTrip<float>(0x6b1f0e2d3c4a5968ull);
    CheckMatrixRoundTrip<double>(0x2c3d4e5f60718293ull);
}

TEST(Transform3_ComposeAndInverseMatchAffineProduct) {
    CheckComposeAndInverse<float>(0x0f1e2d3c4b5a6978ull);
    CheckComposeAndInverse<double>(0x13579bdf2468ace1ull);
}

TEST(Transform3_ShearIsUnsupportedAndZeroScaleIsInvalid) {
    CheckShearAndZeroScale<float>();
    CheckShearAndZeroScale<double>();
}

TEST(Transform3_WideningConversionsAreExact) {
    MathRandom random(0x5eed0b9b1ull ^ CurrentSeed());
    for (int iteration = 0; iteration < 1000; ++iteration) {
        // Transform → Transform3d: 회전·scale float → double은 정확하고, 되돌리면 원래 값이다.
        const Transform authoring(RandomVector<double>(random, 1.0e7),
                                  RandomRotation<float>(random), RandomScale<float>(random, true));
        const Transform3d wide = authoring.ToTransform3d();
        CHECK(wide.translation == authoring.translation);
        CHECK(wide.rotation == Quatd(authoring.rotation));
        CHECK(wide.scale == Vec3d(authoring.scale));
        CHECK(SameComponents(wide.RoundToTransform(), authoring));
        // 둘 다 double로 계산한다. 회전 정규화 방식만 달라(Transform3는 sqrt 한 번의 빠른 경로)
        // 마지막 비트가 다를 수 있고, 이동(최대 1e7, 간격 약 1.9e-9)을 더한 뒤 1 ulp까지 갈린다.
        const Vec3d p = RandomVector<double>(random, 10.0);
        CHECK(NearVector(wide.TransformPoint(p), authoring.TransformPoint(p), 1.0e-8));
        CHECK(wide.ToMatrix().Equals(authoring.ToMatrix(), 1.0e-12));

        // Transform3f → Transform3d·Transform: 정확하고, 어느 경로로 넓혀도 같다.
        const Transform3f single(RandomVector<float>(random, 1.0e5), RandomRotation<float>(random),
                                 RandomScale<float>(random, true));
        const Transform3d singleWide = single.ToTransform3d();
        CHECK(singleWide.translation == Vec3d(single.translation));
        CHECK(singleWide.rotation == Quatd(single.rotation));
        CHECK(singleWide.scale == Vec3d(single.scale));
        CHECK(SameComponents(singleWide.RoundToTransform3f(), single));
        const Transform singleAuthoring = single.ToTransform();
        CHECK(singleAuthoring.translation == Vec3d(single.translation));
        CHECK(singleAuthoring.rotation == single.rotation && singleAuthoring.scale == single.scale);
        CHECK(SameComponents(singleAuthoring.RoundToTransform3f(), single));
        CHECK(SameComponents(singleAuthoring.ToTransform3d(), singleWide));
    }
}

TEST(Transform3_NarrowingConversionsRoundToNearestEven) {
    // 같은 거리면 짝수 가수 쪽, 조금이라도 넘으면 가까운 쪽이다.
    const Transform3d ties(Vec3d(0x1.000001p0, 0x1.000003p0, -0x1.0000010000004p0),
                           Quatd::Identity(),
                           Vec3d(0x1p-150, 0x1.0000000000001p-150, 0x1.fffffefffffffp127));
    const Transform3f rounded = ties.RoundToTransform3f();
    CHECK(rounded.translation == Vec3(1.0f, 0x1.000004p0f, -0x1.000002p0f));
    CHECK(rounded.scale.x == 0.0f && rounded.scale.y == 0x1p-149f &&
          rounded.scale.z == 0x1.fffffep127f);
    // Transform → Transform3f는 이동만 반올림한다(1e9 근처 float 간격은 64).
    const Transform far(Vec3d(1.0e9 + 32.0, 1.0e9 + 33.0, -(1.0e9 + 96.0)), Quat::Identity(),
                        Vec3::One());
    const Transform3f farSingle = far.RoundToTransform3f();
    CHECK(farSingle.translation == Vec3(1.0e9f, 1000000064.0f, -1000000128.0f));
    CHECK(farSingle.rotation == far.rotation && farSingle.scale == far.scale);

    MathRandom random(0xa0b1c2d3e4f50617ull ^ CurrentSeed());
    for (int iteration = 0; iteration < 2000; ++iteration) {
        const Transform3d exact(RandomVector<double>(random, 1.0e6), RandomRotation<double>(random),
                                RandomScale<double>(random, true));
        const Transform3f single = exact.RoundToTransform3f();
        CHECK(IsNearestEvenVector(exact.translation, single.translation));
        CHECK(IsNearestEvenQuaternion(exact.rotation, single.rotation));
        CHECK(IsNearestEvenVector(exact.scale, single.scale));
        // 단위 회전을 좁혀도 길이 오차는 IsValid 허용치 안이다.
        CHECK(single.IsValid());

        // Transform3d → Transform: 이동은 그대로, 회전·scale은 Transform3f와 같은 반올림이다.
        const Transform authoring = exact.RoundToTransform();
        CHECK(authoring.translation == exact.translation);
        CHECK(authoring.rotation == single.rotation && authoring.scale == single.scale);
        CHECK(authoring.IsValid());
        CHECK(authoring.RoundToTransform3f().translation == single.translation);
    }
}

TEST(Transform3_NarrowingOverflowAndUnderflowAreVisible) {
    const float infinity = platform::PlatformMath::Infinity<float>();
    // float 범위를 넘는 성분은 ±Inf가 되어 IsValid가 거짓이다(FLT_MAX와 다음 2의 거듭제곱의 중간은
    // 짝수 쪽인 Inf로 간다).
    const Transform3d huge(Vec3d(1.0e39, 0x1.ffffffp127, -1.0e39), Quatd::Identity(), Vec3d::One());
    CHECK(huge.IsValid());
    const Transform3f hugeSingle = huge.RoundToTransform3f();
    CHECK(hugeSingle.translation == Vec3(infinity, infinity, -infinity));
    CHECK(!hugeSingle.IsValid());
    const Transform authoringFar(Vec3d(1.0e39, 0.0, 0.0), Quat::Identity(), Vec3::One());
    CHECK(authoringFar.IsValid());
    CHECK(!authoringFar.RoundToTransform3f().IsValid());
    const Transform3d hugeScale(Vec3d::Zero(), Quatd::Identity(), Vec3d(1.0e39, 1.0, 1.0));
    CHECK(hugeScale.IsValid());
    CHECK(!hugeScale.RoundToTransform().IsValid());
    CHECK(!hugeScale.RoundToTransform3f().IsValid());

    // float에서 0이 되는 scale은 값으로는 유효하지만 역변환이 InvalidArgument다.
    const Transform3d tinyScale(Vec3d::Zero(), Quatd::Identity(), Vec3d(1.0e-50, 1.0, 1.0));
    CHECK(tinyScale.TryInverse());
    const Transform3f flushed = tinyScale.RoundToTransform3f();
    CHECK(flushed.scale.x == 0.0f && flushed.IsValid());
    CHECK(flushed.TryInverse().Error() == ErrorCode::InvalidArgument);
    CHECK(tinyScale.RoundToTransform().TryInverse().Error() == ErrorCode::InvalidArgument);
    // 분해도 같다: double 분해는 되지만 Transform의 float scale로는 0이다.
    const Mat4d tinyMatrix = Mat4d::Scale(Vec3d(1.0e-50, 1.0, 1.0));
    CHECK(Transform3d::TryFromMatrix(tinyMatrix));
    CHECK(Transform::TryFromMatrix(tinyMatrix).Error() == ErrorCode::InvalidArgument);
}

TEST(Transform3_FloatAndDoubleBoundariesFollowComputePrecision) {
    // 역수가 float로 Inf인 scale(|s| < 1/FLT_MAX ≈ 2.94e-39 — float subnormal의 대부분)은
    // Transform3f가 역변환하지 않는다. 같은 값을 double로 계산하는 Transform·Transform3d는
    // 역변환한다.
    const Transform3f single(Vec3::Zero(), Quat::Identity(), Vec3(1.0e-40f, 1.0f, 1.0f));
    CHECK(single.scale.x != 0.0f && single.IsValid());
    CHECK(single.TryInverseTransformPoint(Vec3::One()).Error() == ErrorCode::InvalidArgument);
    CHECK(single.TryInverseTransformVector(Vec3::One()).Error() == ErrorCode::InvalidArgument);
    CHECK(single.TryInverse().Error() == ErrorCode::InvalidArgument);
    const Transform authoring = single.ToTransform();
    auto authoringBack = authoring.TryInverseTransformPoint(authoring.TransformPoint(Vec3d::One()));
    REQUIRE(authoringBack);
    CHECK(authoringBack.Value() == Vec3d::One());
    auto wideBack = single.ToTransform3d().TryInverseTransformPoint(Vec3d::One());
    REQUIRE(wideBack);
    CHECK(wideBack.Value() == Vec3d(1.0 / static_cast<double>(1.0e-40f), 1.0, 1.0));
    // 1/FLT_MAX보다 큰 subnormal scale은 역수가 유한해 역변환한다. 다만 결과가 float 범위를 넘으면
    // TryInverseTransformPoint는 비유한 성분으로 성공하고(주석에 적은 동작), TryInverse는 역 이동이
    // 비유한이라 InvalidArgument다.
    const Transform3f subnormal(Vec3(1000.0f, 0.0f, 0.0f), Quat::Identity(),
                                Vec3(5.0e-39f, 1.0f, 1.0f));
    auto subnormalBack = subnormal.TryInverseTransformPoint(Vec3(1000.0f, 1.0f, 0.0f));
    REQUIRE(subnormalBack);
    CHECK(subnormalBack.Value() == Vec3(0.0f, 1.0f, 0.0f));
    auto overflowingBack = subnormal.TryInverseTransformPoint(Vec3(2000.0f, 0.0f, 0.0f));
    REQUIRE(overflowingBack);
    CHECK(overflowingBack.Value().x == platform::PlatformMath::Infinity<float>());
    CHECK(subnormal.TryInverse().Error() == ErrorCode::InvalidArgument);
    CHECK(Transform3f(Vec3::Zero(), Quat::Identity(), Vec3(5.0e-39f, 1.0f, 1.0f)).TryInverse());
    // 역수가 double로 Inf인 scale(|s| < 1/DBL_MAX ≈ 5.56e-309)은 Transform3d도 역변환하지 않는다.
    const Transform3d dual(Vec3d::Zero(), Quatd::Identity(), Vec3d(1.0e-310, 1.0, 1.0));
    CHECK(dual.TryInverseTransformPoint(Vec3d::One()).Error() == ErrorCode::InvalidArgument);
    CHECK(Transform3d(Vec3d::Zero(), Quatd::Identity(), Vec3d(1.0e-308, 1.0, 1.0))
              .TryInverseTransformPoint(Vec3d::One()));

    // float 행렬의 분해는 double로 하므로 열 길이가 float 범위를 넘어도 분해 자체는 되고, 결과
    // scale이 float로 표현되지 않으면 InvalidArgument다.
    const float large = 3.0e38f;
    const Mat4 overflowing =
        Mat4::FromColumns(Vec4(large, large, 0.0f, 0.0f), Vec4(-large, large, 0.0f, 0.0f),
                          Vec4(0.0f, 0.0f, 1.0f, 0.0f), Vec4(0.0f, 0.0f, 0.0f, 1.0f));
    CHECK(Transform3f::TryFromMatrix(overflowing).Error() == ErrorCode::InvalidArgument);
    auto overflowingWide = Transform3d::TryFromMatrix(Widen(overflowing));
    REQUIRE(overflowingWide);
    CHECK(Near(overflowingWide.Value().scale.x, 3.0e38 * math::Sqrt(2.0), 0.0, 1.0e-7));
    CHECK(Transform::TryFromMatrix(Widen(overflowing)).Error() == ErrorCode::InvalidArgument);
    // float subnormal scale 행렬은 float로 표현되므로 분해된다.
    CHECK(Transform3f::TryFromMatrix(Mat4::Scale(Vec3(1.0e-40f, 1.0f, 1.0f))));

    // 원점에서 먼 이동: float 간격(1e9 근처 64) 아래의 변위는 Transform3f에서 사라지고 Transform3d·
    // Transform은 지킨다.
    const Transform3d wide(Vec3d(1.0e9, -1.0e9, 5.0), Quatd::Identity(), Vec3d::One());
    CHECK(Near(wide.TransformPoint(Vec3d(0.001, 0.0, 0.0)).x - 1.0e9, 0.001, 1.0e-6));
    const Transform3f narrow = wide.RoundToTransform3f();
    CHECK(narrow.translation.x == 1.0e9f);
    CHECK(narrow.TransformPoint(Vec3(0.001f, 0.0f, 0.0f)).x == 1.0e9f);
    const Transform mixed = wide.RoundToTransform();
    CHECK(mixed.translation == wide.translation);
    CHECK(Near(mixed.TransformPoint(Vec3d(0.001, 0.0, 0.0)).x - 1.0e9, 0.001, 1.0e-6));
}

TEST(Transform3_ExtremeScalesInvertAndDecompose) {
    const Quatd rotation = Quatd::TryFromAxisAngle(Vec3d(1.0, -2.0, 0.5), 0.8).Value();
    const Vec3d p(0.5, -1.5, 2.5);
    // 역변환은 행렬식(s³)을 쓰지 않으므로 TRS 역이 표현되는 극단 scale에서도 된다(4x4 일반
    // 역행렬로는 |s|가 약 1e±102 밖에서 행렬식이 넘쳐 실패했다).
    const double uniformScales[] = {1.0e-250, 1.0e-120, 1.0e120, 1.0e250};
    for (const double uniform : uniformScales) {
        const Transform3d transform(Vec3d::Zero(), rotation, Vec3d::Splat(uniform));
        auto inverse = transform.TryInverse();
        REQUIRE(inverse);
        CHECK(Near(inverse.Value().scale.x, 1.0 / uniform, 0.0, 1.0e-12));
        CHECK(inverse.Value().HasUniformScale(inverse.Value().scale.x * 1.0e-12));
        CHECK(AngularDistance(inverse.Value().rotation, rotation.Conjugate()) < 1.0e-12);
        CHECK(NearVector(inverse.Value().TransformPoint(transform.TransformPoint(p)), p, 1.0e-12));
    }

    const Transform3d moved(Vec3d(1.0, 2.0, 3.0), rotation, Vec3d::Splat(1.0e120));
    auto movedInverse = moved.TryInverse();
    REQUIRE(movedInverse);
    const Vec3d expectedTranslation = -(rotation.UnrotateVector(moved.translation) / 1.0e120);
    for (usize axis = 0; axis < 3; ++axis) {
        CHECK(
            Near(movedInverse.Value().translation[axis], expectedTranslation[axis], 0.0, 1.0e-12));
    }

    // 분해의 열 길이는 제곱이 넘치거나(1e155) subnormal로 떨어지는(1e-160·1e-165) 크기에서도
    // 정확하다.
    const double axisScales[] = {1.0e155, 1.0e300, 1.0e-160, 1.0e-165, 1.0e-300};
    for (const double axisScale : axisScales) {
        const Transform3d original(Vec3d(1.0, 2.0, 3.0), rotation, Vec3d(axisScale, 2.0, 3.0));
        auto decomposed = Transform3d::TryFromMatrix(original.ToMatrix());
        REQUIRE(decomposed);
        CHECK(Near(decomposed.Value().scale.x, axisScale, 0.0, 1.0e-14));
        CHECK(Near(decomposed.Value().scale.y, 2.0, 0.0, 1.0e-14));
        CHECK(AngularDistance(decomposed.Value().rotation, rotation) < 1.0e-12);
        CHECK(decomposed.Value().translation == original.translation);
    }

    // float에서 열 길이의 제곱이 넘치는 scale(1e20 → 1e40 > FLT_MAX)도 double 분해라 왕복한다.
    const Transform3f large(Vec3(1.0f, 2.0f, 3.0f), Quat(rotation), Vec3(1.0e20f, 2.0f, 3.0f));
    auto largeBack = Transform3f::TryFromMatrix(large.ToMatrix());
    REQUIRE(largeBack);
    CHECK(Near(largeBack.Value().scale.x, 1.0e20f, 0.0f, 1.0e-6f));
    CHECK(AngularDistance(Quatd(largeBack.Value().rotation), rotation) < 1.0e-5);
    auto largeInverse =
        Transform3f(Vec3::Zero(), Quat(rotation), Vec3::Splat(1.0e20f)).TryInverse();
    REQUIRE(largeInverse);
    CHECK(Near(largeInverse.Value().scale.x, 1.0e-20f, 0.0f, 1.0e-6f));
}

TEST(Transform3_DecompositionAgreesAcrossPrecisions) {
    // 같은 행렬 값이면 세 Transform의 shear·0 scale 판정이 같고, 성공한 결과는 같은 double 분해를
    // 각자의 정밀도로 반올림한 값이다.
    MathRandom random(0x7a6b5c4d3e2f1001ull ^ CurrentSeed());
    int successes = 0;
    int shears = 0;
    for (int iteration = 0; iteration < 3000; ++iteration) {
        // parent가 균일 scale이거나 child 회전이 없으면 TRS, 아니면 대개 shear다.
        const bool uniformParent = iteration % 2 == 0;
        const Vec3 parentScale = uniformParent
                                     ? Vec3::Splat(static_cast<float>(random.Range(0.5, 2.0)))
                                     : RandomScale<float>(random, true);
        const Transform3f parent(RandomVector<float>(random, 50.0), RandomRotation<float>(random),
                                 parentScale);
        const Quat childRotation =
            iteration % 3 == 0 ? Quat::Identity() : RandomRotation<float>(random);
        const Transform3f child(RandomVector<float>(random, 5.0), childRotation,
                                RandomScale<float>(random, true));
        const Mat4 matrix = parent.ToMatrix() * child.ToMatrix();
        const Mat4d wideMatrix = Widen(matrix);
        auto single = Transform3f::TryFromMatrix(matrix);
        auto dual = Transform3d::TryFromMatrix(wideMatrix);
        auto mixed = Transform::TryFromMatrix(wideMatrix);
        CHECK(!single == !dual && !single == !mixed);
        if (!single || !dual || !mixed) {
            CHECK(!single && !dual && !mixed);
            CHECK(single.Error() == dual.Error() && mixed.Error() == dual.Error());
            shears += single.Error() == ErrorCode::Unsupported ? 1 : 0;
            continue;
        }

        ++successes;
        CHECK(SameComponents(single.Value(), dual.Value().RoundToTransform3f()));
        CHECK(mixed.Value().translation == Vec3d(single.Value().translation));
        CHECK(mixed.Value().rotation == single.Value().rotation);
        CHECK(mixed.Value().scale == single.Value().scale);
    }

    CHECK(successes > 0 && shears > 0);
}

} // namespace rocks::test
