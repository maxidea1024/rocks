#include <Core/Math/Shapes.h>
#include <Tests/Support/Test.h>

using namespace rocks;

namespace {

template <typename T>
T Infinity() noexcept {
    if constexpr (IsSameV<T, float>) {
        return __builtin_huge_valf();
    } else {
        return __builtin_huge_val();
    }
}

template <typename T>
T QuietNaN() noexcept {
    if constexpr (IsSameV<T, float>) {
        return __builtin_nanf("");
    } else {
        return __builtin_nan("");
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

    const Ray<T> ray;
    CHECK(ray.IsValid());
    CHECK(ray.direction == Vector3<T>::Forward());

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
    // 오른손 법칙: X축 점 → Y축 점 순서(위에서 볼 때 반시계)는 +Z normal이다.
    auto counterClockwise = Plane<T>::TryFromPoints(V<T>(0, 0, 0), V<T>(1, 0, 0), V<T>(0, 1, 0));
    REQUIRE(counterClockwise);
    CHECK(counterClockwise.Value().normal == Vector3<T>::Up());
    CHECK(IsPositiveZero(counterClockwise.Value().distance));

    auto clockwise = Plane<T>::TryFromPoints(V<T>(0, 0, 0), V<T>(0, 1, 0), V<T>(1, 0, 0));
    REQUIRE(clockwise);
    CHECK(clockwise.Value().normal == Vector3<T>::Down());

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

    // Z축 +90도: local X → world +Y(왼쪽), local Y → world -X.
    const Quaternion<T> quarterTurn =
        Quaternion<T>::FromUnitAxisAngle(Vector3<T>::Up(), math::HalfPi<T>);
    const OrientedBox<T> turned(V<T>(10, 0, 0), quarterTurn, V<T>(2, 1, 0.5));
    const T tolerance = Tolerance<T>() * 10;
    CHECK(turned.AxisX().Equals(Vector3<T>::Left(), tolerance));
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

TEST(ShapePlaneFromPointsUsesCounterClockwiseWinding) {
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
