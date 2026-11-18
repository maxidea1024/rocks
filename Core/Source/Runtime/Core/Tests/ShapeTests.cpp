#include <Core/Math/Shapes.h>
#include <TestSupport/Test.h>
#include <Platform/FloatingPointEnvironment.h>
#include <Platform/PlatformMath.h>

using namespace rocks;

namespace {

template <typename T>
T Infinity() noexcept {
    if constexpr (IsSameV<T, float>) {
        return platform::PlatformMath::Infinity<float>();
    } else {
        return platform::PlatformMath::Infinity<double>();
    }
}

template <typename T>
T QuietNaN() noexcept {
    if constexpr (IsSameV<T, float>) {
        return platform::PlatformMath::QuietNaN<float>();
    } else {
        return platform::PlatformMath::QuietNaN<double>();
    }
}

template <typename T>
Vector3<T> V(double x, double y, double z) noexcept {
    return Vector3<T>(static_cast<T>(x), static_cast<T>(y), static_cast<T>(z));
}

template <typename T>
bool IsPositiveZero(T value) noexcept {
    return value == static_cast<T>(0) && math::CopySign(static_cast<T>(1), value) > 0;
}

template <typename T>
T Tolerance() noexcept {
    return IsSameV<T, float> ? static_cast<T>(1.0e-5) : static_cast<T>(1.0e-12);
}

template <typename T>
void CheckDefaults() {
    // UE와 달리 모든 기본값은 유효한 shape다. 빈 box만 별도 sentinel이다.
    const Plane<T> plane;
    CHECK(plane.IsValid());
    CHECK(plane.normal == Vector3<T>::Up());
    CHECK(plane.distance == 0);

    // UE TRay 기본 생성자·Init과 같이 원점에서 +Z(0, 0, 1)를 향한다(UE Ray.h:30-34, :66-72).
    const Ray<T> ray;
    CHECK(ray.IsValid());
    CHECK(ray.origin == Vector3<T>::Zero());
    CHECK(ray.direction == V<T>(0, 0, 1));

    const Segment<T> segment;
    CHECK(segment.IsValid());
    CHECK(segment.Length() == 0);

    const Sphere<T> sphere;
    CHECK(sphere.IsValid());
    CHECK(sphere.radius == 0);

    const Box<T> box;
    CHECK(box.IsValid());
    CHECK(!box.IsEmpty());

    const Triangle<T> triangle;
    CHECK(triangle.IsValid());
    CHECK(triangle.IsDegenerate());

    const Capsule<T> capsule;
    CHECK(capsule.IsValid());

    const OrientedBox<T> orientedBox;
    CHECK(orientedBox.IsValid());
    CHECK(orientedBox.rotation == Quaternion<T>::Identity());

    const Box<T> empty = Box<T>::Empty();
    CHECK(empty.IsEmpty());
    CHECK(!empty.IsValid());
}

template <typename T>
void CheckPlaneMakeNormalizesAndKeepsPointSet() {
    auto plane = Plane<T>::TryMake(V<T>(0, 0, 2), static_cast<T>(4));
    REQUIRE(plane);
    CHECK(plane.Value().normal == Vector3<T>::Up());
    CHECK(plane.Value().distance == 2);
    CHECK(plane.Value().SignedDistance(V<T>(1, 2, 2)) == 0);
    CHECK(plane.Value().IsValid());

    // 음수 방향 normal도 같은 점 집합 z = -2를 유지해야 한다.
    auto downward = Plane<T>::TryMake(V<T>(0, 0, -4), static_cast<T>(8));
    REQUIRE(downward);
    CHECK(downward.Value().normal == V<T>(0, 0, -1));
    CHECK(downward.Value().distance == 2);
    CHECK(downward.Value().SignedDistance(V<T>(5, 5, -2)) == 0);

    // 큰 normal도 |normal| 계산이 overflow하지 않고 정규화된다.
    auto huge = Plane<T>::TryMake(V<T>(0, 0, 1.0e30), static_cast<T>(3.0e30));
    REQUIRE(huge);
    CHECK(huge.Value().normal == Vector3<T>::Up());
    CHECK(math::Abs(huge.Value().distance - static_cast<T>(3)) <= Tolerance<T>());

    CHECK(!Plane<T>::TryMake(Vector3<T>::Zero(), static_cast<T>(1)));
    CHECK(!Plane<T>::TryMake(Vector3<T>::Up(), QuietNaN<T>()));
    CHECK(!Plane<T>::TryMake(Vector3<T>::Up(), Infinity<T>()));
    CHECK(!Plane<T>::TryMake(Vector3<T>(Infinity<T>(), 0, 0), static_cast<T>(0)));
}

template <typename T>
void CheckPlaneFromPointNormal() {
    auto plane = Plane<T>::TryFromPointNormal(V<T>(0, 0, 5), V<T>(0, 0, 3));
    REQUIRE(plane);
    CHECK(plane.Value().distance == 5);
    CHECK(plane.Value().SignedDistance(V<T>(1, 2, 7)) == 2);
    CHECK(plane.Value().SignedDistance(V<T>(1, 2, 3)) == -2);
    CHECK(plane.Value().ProjectPoint(V<T>(1, 2, 7)) == V<T>(1, 2, 5));

    const Plane<T> flipped = plane.Value().Flipped();
    CHECK(flipped.normal == V<T>(0, 0, -1));
    CHECK(flipped.distance == -5);
    CHECK(flipped.SignedDistance(V<T>(1, 2, 7)) == -2);

    CHECK(!Plane<T>::TryFromPointNormal(V<T>(0, 0, 5), Vector3<T>::Zero()));
    CHECK(!Plane<T>::TryFromPointNormal(V<T>(QuietNaN<T>(), 0, 0), Vector3<T>::Up()));
}

template <typename T>
void CheckPlaneFromPointsWinding() {
    // UE FPlane(A, B, C) = ((B - A) ^ (C - A)).GetSafeNormal(), W = A | N(UE Plane.h:441-446).
    // 원점 → X축 점 → Y축 점은 왼손 좌표계에서 위(+Z)에서 볼 때 전방 → 오른쪽의 시계 방향이며
    // normal은 +Z다.
    auto clockwiseFromAbove = Plane<T>::TryFromPoints(V<T>(0, 0, 0), V<T>(1, 0, 0), V<T>(0, 1, 0));
    REQUIRE(clockwiseFromAbove);
    CHECK(clockwiseFromAbove.Value().normal == Vector3<T>::Up());
    CHECK(IsPositiveZero(clockwiseFromAbove.Value().distance));

    auto reversed = Plane<T>::TryFromPoints(V<T>(0, 0, 0), V<T>(0, 1, 0), V<T>(1, 0, 0));
    REQUIRE(reversed);
    CHECK(reversed.Value().normal == Vector3<T>::Down());

    // UE 대조 golden(cm): 원점에서 +X를 보는 카메라를 향한 렌더 메시 삼각형 A(100,0,0),
    // B(100,10,0), C(100,0,10). UE 렌더 앞면 normal (C - A) x (B - A) = (-100, 0, 0)은 카메라
    // 쪽이고
    // (MeshDescription.cpp:1311-1313) 화면에서 반시계다. FPlane(A, B, C)는 그 반대인 normal
    // (1, 0, 0), W = 100이다. 렌더 앞면 쪽 평면은 (A, C, B) 순서로 만든다.
    const Vector3<T> meshA = V<T>(100, 0, 0);
    const Vector3<T> meshB = V<T>(100, 10, 0);
    const Vector3<T> meshC = V<T>(100, 0, 10);
    auto uePlane = Plane<T>::TryFromPoints(meshA, meshB, meshC);
    REQUIRE(uePlane);
    CHECK(uePlane.Value().normal == V<T>(1, 0, 0));
    CHECK(uePlane.Value().distance == 100);
    auto renderFrontPlane = Plane<T>::TryFromPoints(meshA, meshC, meshB);
    REQUIRE(renderFrontPlane);
    CHECK(renderFrontPlane.Value().normal == V<T>(-1, 0, 0));
    CHECK(renderFrontPlane.Value().distance == -100);
    CHECK(renderFrontPlane.Value().SignedDistance(Vector3<T>::Zero()) == 100);

    auto raised = Plane<T>::TryFromPoints(V<T>(0, 0, 3), V<T>(1, 0, 3), V<T>(0, 1, 3));
    REQUIRE(raised);
    CHECK(raised.Value().distance == 3);

    // 일직선·중복 점은 normal이 없으므로 실패해야 한다(UE는 영 normal 평면을 조용히 만든다).
    CHECK(!Plane<T>::TryFromPoints(V<T>(0, 0, 0), V<T>(1, 1, 1), V<T>(2, 2, 2)));
    CHECK(!Plane<T>::TryFromPoints(V<T>(1, 2, 3), V<T>(1, 2, 3), V<T>(1, 2, 3)));
    CHECK(!Plane<T>::TryFromPoints(V<T>(0, 0, 0), V<T>(1, 0, 0), V<T>(0, QuietNaN<T>(), 0)));
}

template <typename T>
void CheckPlaneValidityTolerance() {
    // 단위 길이 허용 오차는 길이 1e-5(제곱 길이 2e-5)다.
    CHECK(Plane<T>(V<T>(0, 0, 1.000004), static_cast<T>(0)).IsValid());
    CHECK(!Plane<T>(V<T>(0, 0, 1.00002), static_cast<T>(0)).IsValid());
    CHECK(!Plane<T>(V<T>(0, 0, 2), static_cast<T>(0)).IsValid());
    CHECK(!Plane<T>(Vector3<T>::Up(), QuietNaN<T>()).IsValid());
    CHECK(!Plane<T>(Vector3<T>::Up(), Infinity<T>()).IsValid());
    CHECK(!Plane<T>(V<T>(0, 0, QuietNaN<T>()), static_cast<T>(0)).IsValid());
}

template <typename T>
void CheckRayAndSegment() {
    auto ray = Ray<T>::TryMake(V<T>(1, 2, 3), V<T>(0, 3, 4));
    REQUIRE(ray);
    CHECK(ray.Value().IsValid());
    CHECK(ray.Value().direction.Equals(V<T>(0, 0.6, 0.8), Tolerance<T>()));
    CHECK(ray.Value().PointAt(static_cast<T>(5)).Equals(V<T>(1, 5, 7), Tolerance<T>() * 10));

    CHECK(!Ray<T>::TryMake(V<T>(1, 2, 3), Vector3<T>::Zero()));
    CHECK(!Ray<T>::TryMake(V<T>(QuietNaN<T>(), 0, 0), Vector3<T>::UnitX()));
    CHECK(!Ray<T>::TryMake(Vector3<T>::Zero(), V<T>(Infinity<T>(), 0, 0)));
    CHECK(!Ray<T>(Vector3<T>::Zero(), V<T>(2, 0, 0)).IsValid());
    CHECK(!Ray<T>(V<T>(0, Infinity<T>(), 0), Vector3<T>::UnitX()).IsValid());

    auto segment = Segment<T>::TryMake(V<T>(1, 2, 3), V<T>(4, 6, 3));
    REQUIRE(segment);
    CHECK(segment.Value().Length() == 5);
    CHECK(segment.Value().PointAt(static_cast<T>(0)) == V<T>(1, 2, 3));
    CHECK(segment.Value().PointAt(static_cast<T>(1)) == V<T>(4, 6, 3));
    CHECK(segment.Value().PointAt(static_cast<T>(0.5)) == V<T>(2.5, 4, 3));
    CHECK(segment.Value().Delta() == V<T>(3, 4, 0));
    CHECK(!Segment<T>::TryMake(V<T>(1, 2, 3), V<T>(0, 0, QuietNaN<T>())));

    // 0 길이 segment는 유효한 점이다.
    const Segment<T> point(V<T>(1, 1, 1), V<T>(1, 1, 1));
    CHECK(point.IsValid());
    CHECK(point.Length() == 0);
}

template <typename T>
void CheckSphereAndCapsule() {
    auto sphere = Sphere<T>::TryMake(V<T>(1, 2, 3), static_cast<T>(2));
    REQUIRE(sphere);
    CHECK(sphere.Value().radius == 2);

    CHECK(!Sphere<T>::TryMake(V<T>(1, 2, 3), static_cast<T>(-1)));
    CHECK(!Sphere<T>::TryMake(V<T>(1, 2, 3), QuietNaN<T>()));
    CHECK(!Sphere<T>::TryMake(V<T>(1, 2, 3), Infinity<T>()));
    CHECK(!Sphere<T>::TryMake(V<T>(QuietNaN<T>(), 2, 3), static_cast<T>(1)));

    // -0 반지름은 유효하며 TryMake는 +0으로 정규화한다.
    auto negativeZero = Sphere<T>::TryMake(Vector3<T>::Zero(), static_cast<T>(-0.0));
    REQUIRE(negativeZero);
    CHECK(IsPositiveZero(negativeZero.Value().radius));
    CHECK(Sphere<T>(Vector3<T>::Zero(), static_cast<T>(-0.0)).IsValid());
    CHECK(!Sphere<T>(Vector3<T>::Zero(), static_cast<T>(-1)).IsValid());

    auto capsule = Capsule<T>::TryMake(V<T>(0, 0, 0), V<T>(0, 0, 4), static_cast<T>(1));
    REQUIRE(capsule);
    CHECK(capsule.Value().Axis().end == V<T>(0, 0, 4));
    CHECK(!Capsule<T>::TryMake(V<T>(0, 0, 0), V<T>(0, 0, 4), static_cast<T>(-0.5)));
    CHECK(!Capsule<T>::TryMake(V<T>(0, 0, 0), V<T>(0, 0, Infinity<T>()), static_cast<T>(1)));
    CHECK(!Capsule<T>::TryMake(V<T>(0, 0, 0), V<T>(0, 0, 4), QuietNaN<T>()));

    auto capsuleZero = Capsule<T>::TryMake(V<T>(1, 1, 1), V<T>(1, 1, 1), static_cast<T>(-0.0));
    REQUIRE(capsuleZero);
    CHECK(IsPositiveZero(capsuleZero.Value().radius));
}

template <typename T>
void CheckBoxConstruction() {
    auto box = Box<T>::TryMake(V<T>(0, 0, 0), V<T>(1, 2, 3));
    REQUIRE(box);
    CHECK(box.Value().Center() == V<T>(0.5, 1, 1.5));
    CHECK(box.Value().HalfExtents() == V<T>(0.5, 1, 1.5));
    CHECK(box.Value().Size() == V<T>(1, 2, 3));

    // 역전된 box는 자동 정렬하지 않고 거부한다. 0 부피 box는 유효하다.
    CHECK(!Box<T>::TryMake(V<T>(1, 0, 0), V<T>(0, 1, 1)));
    CHECK(Box<T>::TryMake(V<T>(1, 1, 1), V<T>(1, 1, 1)));
    CHECK(Box<T>::TryMake(V<T>(-0.0, 0, 0), V<T>(0, 0, 0)));
    CHECK(!Box<T>::TryMake(V<T>(0, 0, QuietNaN<T>()), V<T>(1, 1, 1)));
    CHECK(!Box<T>::TryMake(V<T>(0, 0, 0), V<T>(1, 1, Infinity<T>())));

    const Box<T> inverted(V<T>(1, 0, 0), V<T>(0, 1, 1));
    CHECK(inverted.IsEmpty());
    CHECK(!inverted.IsValid());

    // NaN box는 비어 있다고도 유효하다고도 보지 않는다.
    const Box<T> poisoned(V<T>(QuietNaN<T>(), 0, 0), V<T>(1, 1, 1));
    CHECK(!poisoned.IsEmpty());
    CHECK(!poisoned.IsValid());

    auto centered = Box<T>::TryFromCenterHalfExtents(V<T>(1, 1, 1), V<T>(1, 2, 0));
    REQUIRE(centered);
    CHECK(centered.Value().min == V<T>(0, -1, 1));
    CHECK(centered.Value().max == V<T>(2, 3, 1));
    CHECK(!Box<T>::TryFromCenterHalfExtents(V<T>(1, 1, 1), V<T>(1, -2, 0)));
    CHECK(!Box<T>::TryFromCenterHalfExtents(V<T>(1, 1, 1), V<T>(1, QuietNaN<T>(), 0)));

    // 큰 유한 좌표에서도 Center는 overflow하지 않는다.
    const T large = IsSameV<T, float> ? static_cast<T>(3.0e38) : static_cast<T>(1.0e308);
    const Box<T> wide(Vector3<T>::Splat(-large), Vector3<T>::Splat(large));
    CHECK(wide.IsValid());
    CHECK(wide.Center() == Vector3<T>::Zero());
    CHECK(wide.HalfExtents() == Vector3<T>::Splat(large));
}

template <typename T>
void CheckBoxFromPoints() {
    CHECK(!Box<T>::TryFromPoints(Span<const Vector3<T>>()));

    const Vector3<T> points[] = {V<T>(1, -2, 3), V<T>(-1, 5, 0), V<T>(0, 0, 7)};
    auto box = Box<T>::TryFromPoints(Span<const Vector3<T>>(points));
    REQUIRE(box);
    CHECK(box.Value().min == V<T>(-1, -2, 0));
    CHECK(box.Value().max == V<T>(1, 5, 7));

    const Vector3<T> single[] = {V<T>(4, 5, 6)};
    auto point = Box<T>::TryFromPoints(Span<const Vector3<T>>(single));
    REQUIRE(point);
    CHECK(point.Value().min == point.Value().max);

    const Vector3<T> withNaN[] = {V<T>(1, 2, 3), V<T>(0, QuietNaN<T>(), 0)};
    CHECK(!Box<T>::TryFromPoints(Span<const Vector3<T>>(withNaN)));

    const Box<T> grown = Box<T>::Empty().ExpandedToInclude(V<T>(2, 3, 4));
    CHECK(grown.IsValid());
    CHECK(grown.min == V<T>(2, 3, 4));
    CHECK(grown.max == V<T>(2, 3, 4));
    CHECK(!Box<T>::Empty().ExpandedToInclude(V<T>(QuietNaN<T>(), 0, 0)).IsValid());
}

template <typename T>
void CheckTriangle() {
    auto triangle = Triangle<T>::TryMake(V<T>(0, 0, 0), V<T>(1, 0, 0), V<T>(0, 1, 0));
    REQUIRE(triangle);
    CHECK(!triangle.Value().IsDegenerate());
    auto normal = triangle.Value().TryNormal();
    REQUIRE(normal);
    CHECK(normal.Value() == Vector3<T>::Up());

    // 퇴화 삼각형은 유효한 값이지만 normal이 없다.
    const Triangle<T> collinear(V<T>(0, 0, 0), V<T>(1, 1, 1), V<T>(2, 2, 2));
    CHECK(collinear.IsValid());
    CHECK(collinear.IsDegenerate());
    CHECK(!collinear.TryNormal());

    const Triangle<T> repeated(V<T>(1, 2, 3), V<T>(1, 2, 3), V<T>(4, 5, 6));
    CHECK(repeated.IsDegenerate());

    CHECK(!Triangle<T>::TryMake(V<T>(0, 0, 0), V<T>(1, 0, 0), V<T>(0, Infinity<T>(), 0)));
}

template <typename T>
void CheckOrientedBox() {
    // 길이 2인 항등 quaternion은 TryMake가 정규화한다.
    auto box = OrientedBox<T>::TryMake(V<T>(1, 2, 3), Quaternion<T>(0, 0, 0, 2), V<T>(1, 2, 3));
    REQUIRE(box);
    CHECK(box.Value().rotation == Quaternion<T>::Identity());
    CHECK(box.Value().IsValid());

    CHECK(!OrientedBox<T>::TryMake(V<T>(1, 2, 3), Quaternion<T>(0, 0, 0, 0), V<T>(1, 2, 3)));
    CHECK(!OrientedBox<T>::TryMake(V<T>(1, 2, 3), Quaternion<T>::Identity(), V<T>(1, -2, 3)));
    CHECK(!OrientedBox<T>::TryMake(V<T>(QuietNaN<T>(), 2, 3), Quaternion<T>::Identity(),
                                   V<T>(1, 2, 3)));

    CHECK(!OrientedBox<T>(V<T>(0, 0, 0), Quaternion<T>(0, 0, 0, 2), V<T>(1, 1, 1)).IsValid());
    CHECK(OrientedBox<T>(V<T>(0, 0, 0), Quaternion<T>(0, 0, 0, static_cast<T>(1.000004)),
                         V<T>(1, 1, 1))
              .IsValid());
    CHECK(!OrientedBox<T>(V<T>(0, 0, 0), Quaternion<T>(0, 0, 0, static_cast<T>(1.00002)),
                          V<T>(1, 1, 1))
               .IsValid());
    CHECK(!OrientedBox<T>(V<T>(0, 0, 0), Quaternion<T>::Identity(), V<T>(1, -0.5, 1)).IsValid());

    // Z축 +90도(UE 왼손 양의 각): local X → world +Y(오른쪽), local Y → world -X.
    const Quaternion<T> quarterTurn =
        Quaternion<T>::FromUnitAxisAngle(Vector3<T>::Up(), math::HalfPi<T>);
    const OrientedBox<T> turned(V<T>(10, 0, 0), quarterTurn, V<T>(2, 1, 0.5));
    const T tolerance = Tolerance<T>() * 10;
    CHECK(turned.AxisX().Equals(Vector3<T>::Right(), tolerance));
    CHECK(turned.AxisY().Equals(Vector3<T>::Backward(), tolerance));
    CHECK(turned.AxisZ().Equals(Vector3<T>::Up(), tolerance));
    CHECK(turned.ToWorld(V<T>(2, 0, 0)).Equals(V<T>(10, 2, 0), tolerance * 10));
    CHECK(turned.ToLocal(V<T>(10, 2, 0)).Equals(V<T>(2, 0, 0), tolerance * 10));
    CHECK(turned.Corner(0).Equals(V<T>(11, -2, -0.5), tolerance * 10));
    CHECK(turned.Corner(7).Equals(V<T>(9, 2, 0.5), tolerance * 10));
}

template <typename T>
void CheckDefaultValueConstructorsDoNotValidate() {
    // 값 생성자는 검사하지 않으므로 잘못된 값도 표현할 수 있고 IsValid가 그것을 드러낸다.
    CHECK(!Sphere<T>(Vector3<T>::Zero(), static_cast<T>(-2)).IsValid());
    CHECK(!Capsule<T>(Vector3<T>::Zero(), Vector3<T>::One(), static_cast<T>(-2)).IsValid());
    CHECK(!Segment<T>(Vector3<T>::Zero(), V<T>(QuietNaN<T>(), 0, 0)).IsValid());
    CHECK(!Triangle<T>(Vector3<T>::Zero(), V<T>(QuietNaN<T>(), 0, 0), Vector3<T>::One()).IsValid());
}

} // namespace

TEST(ShapeDefaultsAreValidAndEmptyBoxIsNot) {
    CheckDefaults<float>();
    CheckDefaults<double>();
}

TEST(ShapePlaneMakeNormalizesAndKeepsPointSet) {
    CheckPlaneMakeNormalizesAndKeepsPointSet<float>();
    CheckPlaneMakeNormalizesAndKeepsPointSet<double>();
}

TEST(ShapePlaneFromPointNormal) {
    CheckPlaneFromPointNormal<float>();
    CheckPlaneFromPointNormal<double>();
}

TEST(ShapePlaneFromPointsMatchesUePlaneNotRenderFront) {
    CheckPlaneFromPointsWinding<float>();
    CheckPlaneFromPointsWinding<double>();
}

TEST(ShapePlaneValidityTolerance) {
    CheckPlaneValidityTolerance<float>();
    CheckPlaneValidityTolerance<double>();
}

TEST(ShapeRayAndSegmentConstruction) {
    CheckRayAndSegment<float>();
    CheckRayAndSegment<double>();
}

TEST(ShapeSphereAndCapsuleConstruction) {
    CheckSphereAndCapsule<float>();
    CheckSphereAndCapsule<double>();
}

TEST(ShapeBoxConstructionRejectsInvertedAndKeepsZeroVolume) {
    CheckBoxConstruction<float>();
    CheckBoxConstruction<double>();
}

TEST(ShapeBoxFromPointsRejectsEmptyAndNaN) {
    CheckBoxFromPoints<float>();
    CheckBoxFromPoints<double>();
}

TEST(ShapeTriangleNormalAndDegeneracy) {
    CheckTriangle<float>();
    CheckTriangle<double>();
}

TEST(ShapeOrientedBoxConstructionAndAxes) {
    CheckOrientedBox<float>();
    CheckOrientedBox<double>();
}

TEST(ShapeValueConstructorsDoNotValidate) {
    CheckDefaultValueConstructorsDoNotValidate<float>();
    CheckDefaultValueConstructorsDoNotValidate<double>();
}

TEST(ShapePrecisionConversionIsExplicitAndStaysValid) {
    // float로 정규화한 방향·회전은 double로 바꿔도 단위 허용 오차 안에 남아야 한다.
    auto narrowRay = Ray<float>::TryMake(Vec3(1, 2, 3), Vec3(1, 1, 1));
    REQUIRE(narrowRay);
    const Ray<double> wideRay(narrowRay.Value());
    CHECK(wideRay.IsValid());
    CHECK(wideRay.origin == Vec3d(1, 2, 3));

    const Quat rotation = Quat::FromUnitAxisAngle(Vec3(0, 0, 1), 0.7f);
    const OrientedBox<double> wideBox(OrientedBox<float>(Vec3(1, 2, 3), rotation, Vec3(1, 1, 1)));
    CHECK(wideBox.IsValid());

    const Sphere<double> wideSphere(Sphere<float>(Vec3(1, 2, 3), 0.5f));
    CHECK(wideSphere.center == Vec3d(1, 2, 3));
    CHECK(wideSphere.radius == 0.5);

    const Box<float> narrowBox(Box<double>(Vec3d(-1, -2, -3), Vec3d(1, 2, 3)));
    CHECK(narrowBox.IsValid());
    CHECK(narrowBox.max == Vec3(1, 2, 3));

    const Plane<float> narrowPlane(Plane<double>(Vec3d(0, 0, 1), 2.0));
    CHECK(narrowPlane.IsValid());
    CHECK(narrowPlane.distance == 2.0f);

    const Triangle<double> wideTriangle(
        Triangle<float>(Vec3(0, 0, 0), Vec3(1, 0, 0), Vec3(0, 1, 0)));
    CHECK(wideTriangle.b == Vec3d(1, 0, 0));

    const Capsule<double> wideCapsule(Capsule<float>(Vec3(0, 0, 0), Vec3(0, 0, 1), 0.25f));
    CHECK(wideCapsule.radius == 0.25);

    const Segment<double> wideSegment(Segment<float>(Vec3(0, 0, 0), Vec3(0, 0, 1)));
    CHECK(wideSegment.end == Vec3d(0, 0, 1));
}

namespace {

template <typename T>
void CheckBoxContainmentUnionAndExpansion() {
    const Box<T> outer(V<T>(-2, -2, -2), V<T>(2, 2, 2));

    // 닫힌 집합: 면에 닿는 box와 크기 0 box도 포함이다.
    CHECK(outer.Contains(Box<T>(V<T>(-2, -1, 0), V<T>(2, 1, 2))));
    CHECK(outer.Contains(Box<T>(V<T>(2, 2, 2), V<T>(2, 2, 2))));
    CHECK(outer.Contains(outer));
    CHECK(!outer.Contains(Box<T>(V<T>(-2, -1, 0), V<T>(2.5, 1, 2))));
    CHECK(!outer.Contains(Box<T>(V<T>(-3, -3, -3), V<T>(-2.5, -2.5, -2.5))));
    // 역전·NaN·무한대 box는 어떤 box에도 들어 있지 않다.
    CHECK(!outer.Contains(Box<T>(V<T>(1, 0, 0), V<T>(0, 0, 0))));
    CHECK(!outer.Contains(Box<T>(V<T>(QuietNaN<T>(), 0, 0), V<T>(0, 0, 0))));
    const Box<T> everything(Vector3<T>::Splat(-Infinity<T>()), Vector3<T>::Splat(Infinity<T>()));
    CHECK(!everything.Contains(Box<T>(V<T>(0, 0, 0), V<T>(Infinity<T>(), 0, 0))));

    CHECK(outer.Contains(V<T>(2, -2, 0)));
    CHECK(!outer.Contains(V<T>(2.5, 0, 0)));
    CHECK(!outer.Contains(V<T>(QuietNaN<T>(), 0, 0)));

    const Box<T> left(V<T>(-5, 0, 1), V<T>(-4, 1, 2));
    const Box<T> right(V<T>(3, -1, 0), V<T>(4, 0.5, 6));
    const Box<T> both = left.Union(right);
    CHECK(both.min == V<T>(-5, -1, 0));
    CHECK(both.max == V<T>(4, 1, 6));
    CHECK(both.Contains(left) && both.Contains(right));
    CHECK(Box<T>::Empty().Union(left).min == left.min);
    CHECK(Box<T>::Empty().Union(left).max == left.max);
    // NaN은 합집합에서 사라지지 않는다.
    CHECK(!left.Union(Box<T>(V<T>(QuietNaN<T>(), 0, 0), V<T>(0, 0, 0))).IsValid());

    const Box<T> grown = left.ExpandedBy(V<T>(1, 2, 0.5));
    CHECK(grown.min == V<T>(-6, -2, 0.5));
    CHECK(grown.max == V<T>(-3, 3, 2.5));
    CHECK(!left.ExpandedBy(V<T>(-0.5, 0, 0)).IsEmpty());
    CHECK(left.ExpandedBy(V<T>(-0.75, 0, 0)).IsEmpty());
}

// 방향 반올림: 아래로 내린 값은 원래 값 이하, 올린 값은 이상이며 둘 다 가장 가까운 float다.
void CheckDirectedFloatRounding(double value) {
    const float down = math::ToFloatRoundedDown(value);
    const float up = math::ToFloatRoundedUp(value);
    CHECK(static_cast<double>(down) <= value);
    CHECK(static_cast<double>(up) >= value);
    CHECK(static_cast<double>(math::NextUp(down)) > value || static_cast<double>(down) == value);
    CHECK(static_cast<double>(math::NextDown(up)) < value || static_cast<double>(up) == value);
    if (static_cast<double>(static_cast<float>(value)) == value) {
        CHECK(down == up);
    } else {
        CHECK(math::NextUp(down) == up);
    }
}

} // namespace

TEST(ShapeBoxContainmentUnionAndExpansion) {
    CheckBoxContainmentUnionAndExpansion<float>();
    CheckBoxContainmentUnionAndExpansion<double>();
}

TEST(MathNextUpDownStepsOneUlpAndHandlesSpecialValues) {
    CHECK(math::NextUp(1.0f) == 1.0f + math::MachineEpsilon<float>);
    CHECK(math::NextDown(1.0f) == 1.0f - math::MachineEpsilon<float> / 2);
    CHECK(math::NextUp(-1.0) == -1.0 + math::MachineEpsilon<double> / 2);
    CHECK(math::NextDown(-1.0) == -1.0 - math::MachineEpsilon<double>);
    // 0의 이웃은 가장 작은 비정규 값이며 +0·-0 모두 같다.
    CHECK(math::NextUp(0.0f) > 0.0f && math::NextUp(0.0f) < NumericLimits<float>::Min());
    CHECK(math::NextUp(-0.0f) == math::NextUp(0.0f));
    CHECK(math::NextDown(0.0) == -math::NextUp(0.0));
    CHECK(math::NextUp(NumericLimits<float>::Max()) == Infinity<float>());
    CHECK(math::NextUp(Infinity<float>()) == Infinity<float>());
    CHECK(math::NextUp(-Infinity<double>()) == NumericLimits<double>::Lowest());
    CHECK(math::NextDown(-Infinity<float>()) == -Infinity<float>());
    CHECK(math::IsNaN(math::NextUp(QuietNaN<float>())));
    CHECK(math::IsNaN(math::NextDown(QuietNaN<double>())));
    static_assert(math::NextUp(1.0f) > 1.0f, "NextUp must be usable in constant evaluation");
}

TEST(MathDirectedFloatRoundingBracketsDoubleValues) {
    // 최근접 반올림이 위로 가는 값(1 + 2^-24 + 2^-40 → 1 + 2^-23)과 아래로 가는 값, 정확한 값,
    // 음수.
    const double values[] = {1.0 + 0x1.0p-24 + 0x1.0p-40,
                             1.0 + 0x1.0p-25,
                             1.0,
                             -1.0 - 0x1.0p-30,
                             0.1,
                             -0.1,
                             1.0e-42,
                             -1.0e-42,
                             123456789.123,
                             -987654321.5};
    for (const double value : values) {
        CheckDirectedFloatRounding(value);
    }

    // float 범위를 넘는 값은 포함 관계를 지키는 쪽으로 끝값·무한대가 된다.
    CHECK(math::ToFloatRoundedDown(1.0e300) == NumericLimits<float>::Max());
    CHECK(math::ToFloatRoundedUp(1.0e300) == Infinity<float>());
    CHECK(math::ToFloatRoundedDown(-1.0e300) == -Infinity<float>());
    CHECK(math::ToFloatRoundedUp(-1.0e300) == NumericLimits<float>::Lowest());
    // 무한대는 float에서도 표현되므로 그대로다(가장 가까운 아래·위 값이 자기 자신).
    CHECK(math::ToFloatRoundedDown(Infinity<double>()) == Infinity<float>());
    CHECK(math::ToFloatRoundedUp(-Infinity<double>()) == -Infinity<float>());
    CHECK(math::IsNaN(math::ToFloatRoundedDown(QuietNaN<double>())));

    const Box<double> wide(Vec3d(0.1, -0.1, 1.0e10 + 0.5), Vec3d(0.3, 1.0 / 3.0, 1.0e10 + 1.5));
    const Box<float> narrow = wide.ToFloatOutward();
    CHECK(static_cast<double>(narrow.min.x) <= wide.min.x);
    CHECK(static_cast<double>(narrow.min.y) <= wide.min.y);
    CHECK(static_cast<double>(narrow.min.z) <= wide.min.z);
    CHECK(static_cast<double>(narrow.max.x) >= wide.max.x);
    CHECK(static_cast<double>(narrow.max.y) >= wide.max.y);
    CHECK(static_cast<double>(narrow.max.z) >= wide.max.z);
    CHECK(Box<double>(narrow).Contains(wide));
}

namespace {

uint32 Bits(float value) noexcept {
    return platform::BitCast<uint32>(value);
}

// 방향 반올림의 비정규·정규 경계 입력과 기대 비트다(아래, 위). 기대값은 IEEE 754 정의에서 손으로
// 구한 것이다: 2^-149는 가장 작은 비정규 float(비트 1), 2^-126은 가장 작은 정규 float(비트
// 0x00800000)다.
struct DirectedCase {
    double value;
    uint32 down;
    uint32 up;
};

const DirectedCase DirectedCases[] = {
    {0x1.0p-149, 0x00000001u, 0x00000001u},
    {0x1.0p-150, 0x00000000u, 0x00000001u},
    {-0x1.0p-150, 0x80000001u, 0x80000000u},
    {0x1.8p-149, 0x00000001u, 0x00000002u},
    {0x1.0p-1074, 0x00000000u, 0x00000001u},
    {-0x1.0p-1074, 0x80000001u, 0x80000000u},
    {0x0.fffffep-126, 0x007fffffu, 0x007fffffu},
    {0x0.ffffffp-126, 0x007fffffu, 0x00800000u},
    {0x1.0p-126, 0x00800000u, 0x00800000u},
    {-0x0.ffffffp-126, 0x80800000u, 0x807fffffu},
    {0.0, 0x00000000u, 0x00000000u},
    {-0.0, 0x80000000u, 0x80000000u},
    {0x1.fffffep127, 0x7f7fffffu, 0x7f7fffffu},
    {0x1.ffffffp127, 0x7f7fffffu, 0x7f800000u},
    {-0x1.ffffffp127, 0xff800000u, 0xff7fffffu},
};

bool DirectedCasesMatch() noexcept {
    bool same = true;
    for (const DirectedCase& entry : DirectedCases) {
        same = same && Bits(math::ToFloatRoundedDown(entry.value)) == entry.down &&
               Bits(math::ToFloatRoundedUp(entry.value)) == entry.up;
    }
    // 비정규 값의 이웃(NextUp·NextDown)도 비트 연산이다.
    same = same && Bits(math::NextDown(platform::BitCast<float>(0x00000001u))) == 0 &&
           Bits(math::NextUp(platform::BitCast<float>(0x807fffffu))) == 0x807ffffeu &&
           Bits(math::NextUp(platform::BitCast<float>(0x007fffffu))) == 0x00800000u;
    return same;
}

} // namespace

// 비정규 경계: 방향 반올림은 비정규 값까지 정확하다(정수 비트 연산).
TEST(MathDirectedFloatRoundingSubnormalBoundaries) {
    CHECK(DirectedCasesMatch());
    static_assert(math::ToFloatRoundedUp(0x1.0p-150) > 0.0f,
                  "Directed rounding must be usable in constant evaluation");
}

// FTZ·DAZ를 켜도 같은 비트를 낸다. 부동소수 비교가 비정규 입력을 0으로 보는 환경에서도 계약("항상
// value 이하/이상")이 깨지지 않아야 공간 인덱스의 외향 보정이 성립한다.
TEST(MathDirectedFloatRoundingIgnoresFlushToZero) {
    const uint64 original = platform::detail::ReadFloatingPointControl();
    platform::detail::WriteFloatingPointControl(original | platform::detail::FlushToZeroBit() |
                                                platform::detail::DenormalsAreZeroBit());
    const bool same = DirectedCasesMatch();
    platform::detail::WriteFloatingPointControl(original);
    CHECK(same);
}
