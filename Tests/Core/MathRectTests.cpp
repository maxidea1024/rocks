#include "Tests/Support/Test.h"
#include <Core/Math/Interval.h>
#include <Core/Math/Rect.h>

namespace rocks::test {

TEST(Box2_ClosedBoundaryContracts) {
    const Box2<double> box(Vec2d(0.0, 0.0), Vec2d(4.0, 2.0));
    CHECK(box.IsValid());
    CHECK(box.Area() == 8.0 && box.Center() == Vec2d(2.0, 1.0) && box.Extent() == Vec2d(2.0, 1.0));
    // 닫힌 집합: 경계 위 점은 Contains, 엄격 판정에서는 제외(UE IsInsideOrOn/IsInside).
    CHECK(box.Contains(Vec2d(4.0, 2.0)) && !box.ContainsStrictly(Vec2d(4.0, 2.0)));
    CHECK(box.ContainsStrictly(Vec2d(1.0, 1.0)));
    CHECK(!box.Contains(Vec2d(4.0000000001, 1.0)));
    CHECK(!box.Contains(Vec2d(__builtin_nan(""), 1.0)));
    // 변 접촉은 겹침이고 교집합은 넓이 0인 선분이다.
    const Box2<double> touching(Vec2d(4.0, -1.0), Vec2d(5.0, 1.0));
    CHECK(box.Overlaps(touching) && touching.Overlaps(box));
    auto intersection = box.TryIntersection(touching);
    REQUIRE(intersection);
    CHECK(intersection.Value() == Box2<double>(Vec2d(4.0, 0.0), Vec2d(4.0, 1.0)));
    CHECK(intersection.Value().Area() == 0.0 && intersection.Value().IsValid());
    const Box2<double> apart(Vec2d(5.0, 5.0), Vec2d(6.0, 6.0));
    CHECK(!box.Overlaps(apart));
    CHECK(box.TryIntersection(apart).Error() == ErrorCode::NotFound);
    CHECK(box.Union(apart) == Box2<double>(Vec2d(0.0, 0.0), Vec2d(6.0, 6.0)));
    CHECK(box.Contains(Box2<double>(Vec2d(1.0, 1.0), Vec2d(4.0, 2.0))));
    CHECK(box.ClosestPoint(Vec2d(10.0, -3.0)) == Vec2d(4.0, 0.0));
    CHECK(box.ExpandedToInclude(Vec2d(-1.0, 0.5)).min == Vec2d(-1.0, 0.0));
    CHECK(!box.ExpandedBy(Vec2d(-3.0, 0.0)).IsValid()); // 뒤집힌 결과
}

TEST(Box2_ConstructionValidation) {
    CHECK(!Box2<float>::TryMake(Vec2(1.0f, 0.0f), Vec2(0.0f, 1.0f)));
    CHECK(!Box2<float>::TryMake(Vec2(0.0f, 0.0f), Vec2(__builtin_huge_valf(), 1.0f)));
    CHECK(Box2<float>::TryMake(Vec2(1.0f, 1.0f), Vec2(1.0f, 1.0f))); // 점 경계
    const Vec2d points[] = {Vec2d(3.0, -1.0), Vec2d(-2.0, 5.0), Vec2d(0.5, 0.5)};
    auto fromPoints = Box2<double>::TryFromPoints(Span<const Vec2d>(points));
    REQUIRE(fromPoints);
    CHECK(fromPoints.Value() == Box2<double>(Vec2d(-2.0, -1.0), Vec2d(3.0, 5.0)));
    CHECK(!Box2<double>::TryFromPoints(Span<const Vec2d>()));
    const Vec2d bad[] = {Vec2d(0.0, 0.0), Vec2d(__builtin_nan(""), 1.0)};
    CHECK(!Box2<double>::TryFromPoints(Span<const Vec2d>(bad)));
}

TEST(IntRect_HalfOpenPixelContracts) {
    const IntRect rect(IntVec2(0, 0), IntVec2(4, 3));
    CHECK(rect.Size() == IntVec2(4, 3) && rect.Area() == 12);
    CHECK(rect.Contains(IntVec2(0, 0)) && rect.Contains(IntVec2(3, 2)));
    CHECK(!rect.Contains(IntVec2(4, 2)) && !rect.Contains(IntVec2(3, 3)));
    // 변만 닿는 사각형은 겹치지 않는다(반열린 구간).
    CHECK(!rect.Overlaps(IntRect(IntVec2(4, 0), IntVec2(5, 3))));
    CHECK(rect.Overlaps(IntRect(IntVec2(3, 2), IntVec2(5, 5))));
    CHECK(IntRect(IntVec2(2, 2), IntVec2(2, 9)).IsEmpty());
    CHECK(!IntRect(IntVec2(3, 0), IntVec2(2, 1)).IsValid());
    // int32 전체 폭의 넓이도 int64로 정확히 계산한다.
    const IntRect huge(IntVec2(-2147483647 - 1, 0), IntVec2(2147483647, 2));
    CHECK(huge.Area() == 2ll * 4294967295ll);
}

TEST(Interval_ClosedRangeContracts) {
    const Interval<double> range(2.0, 6.0);
    CHECK(range.IsValid() && range.Size() == 4.0);
    CHECK(range.Contains(2.0) && range.Contains(6.0) && !range.Contains(6.5));
    CHECK(!range.Contains(__builtin_nan("")));
    CHECK(range.Clamp(9.0) == 6.0 && range.Clamp(-1.0) == 2.0);
    CHECK(range.Interpolate(0.0) == 2.0 && range.Interpolate(1.0) == 6.0);
    CHECK(range.Interpolate(0.25) == 3.0);
    CHECK(range.ExpandedToInclude(-3.0) == Interval<double>(-3.0, 6.0));
    CHECK(!range.ExpandedToInclude(__builtin_nan("")).IsValid());
    auto touching = range.TryIntersection(Interval<double>(6.0, 8.0));
    REQUIRE(touching);
    CHECK(touching.Value() == Interval<double>(6.0, 6.0));
    CHECK(range.TryIntersection(Interval<double>(7.0, 8.0)).Error() == ErrorCode::NotFound);
    CHECK(!Interval<float>::TryMake(1.0f, 0.0f));
    CHECK(!Interval<float>::TryMake(0.0f, __builtin_huge_valf()));
    CHECK(Interval<float>::TryMake(1.0f, 1.0f));
}

TEST(Rect_ReviewRegressions) {
    // 검토에서 확인한 반례: 전체 int32 범위 넓이는 int64를 넘는다(이전 구현은 부호 overflow UB).
    const IntRect full(IntVec2(-2147483647 - 1, -2147483647 - 1), IntVec2(2147483647, 2147483647));
    CHECK(full.Area() == 18446744065119617025ull);
    // 빈·뒤집힌 사각형은 겹치지 않는다.
    const IntRect square(IntVec2(0, 0), IntVec2(10, 10));
    CHECK(!square.Overlaps(IntRect(IntVec2(5, 0), IntVec2(5, 10))));
    CHECK(!square.Overlaps(IntRect(IntVec2(8, 8), IntVec2(2, 2))));
    CHECK(!IntRect(IntVec2(8, 8), IntVec2(2, 2)).Overlaps(square));
    // NaN은 누적 다음 단계에서도 사라지지 않는다.
    const double nan = __builtin_nan("");
    const Box2<double> box(Vec2d(0.0, 0.0), Vec2d(1.0, 1.0));
    const Box2<double> poisoned = box.ExpandedToInclude(Vec2d(nan, 0.5));
    CHECK(!poisoned.IsValid());
    CHECK(!poisoned.ExpandedToInclude(Vec2d(5.0, 5.0)).IsValid());
    CHECK(!poisoned.Union(box).IsValid() && !box.Union(poisoned).IsValid());
    CHECK(box.TryIntersection(poisoned).Error() == ErrorCode::InvalidArgument);
    CHECK(!box.Contains(Box2<double>(Vec2d(0.8, 0.8), Vec2d(0.2, 0.2)))); // 뒤집힌 box
    const Vec2d closest = box.ClosestPoint(Vec2d(nan, 3.0));
    CHECK(math::IsNaN(closest.x) && closest.y == 1.0);
    const Interval<double> range(0.0, 1.0);
    const Interval<double> nanRange = range.ExpandedToInclude(nan);
    CHECK(!nanRange.IsValid() && !nanRange.ExpandedToInclude(5.0).IsValid());
    CHECK(nanRange.TryIntersection(range).Error() == ErrorCode::InvalidArgument);
    CHECK(range.TryIntersection(nanRange).Error() == ErrorCode::InvalidArgument);
    // 차이가 overflow하는 넓은 float 구간도 [0,1] 보간 결과는 유한하다.
    const Interval<float> wide(-3.40282347e+38f, 3.40282347e+38f);
    CHECK(wide.IsValid() && wide.Interpolate(0.0f) == wide.min &&
          wide.Interpolate(1.0f) == wide.max);
    CHECK(wide.Interpolate(0.5f) == 0.0f && math::IsFinite(wide.Interpolate(0.25f)));
}

} // namespace rocks::test
