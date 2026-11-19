#pragma once

#include <Core/Math/IntersectionCast.h>
#include <Core/Math/IntersectionContainment.h>

namespace rocks {

// 겹침 판정(Overlap)이다. 볼록 영역(ConvexVolume)과의 보수적 겹침도 여기에 있다.

// ===== 겹침 =====
// 서로 다른 타입 쌍은 두 인자 순서를 모두 제공한다.

// |c1 - c2|² <= (r1 + r2)². UE TSphere::Intersects와 달리 기본 허용 오차(1e-4)를 더하지 않는다.
template <typename T>
QueryResult Overlap(const Sphere<T>& first, const Sphere<T>& second) noexcept {
    if (!first.IsValid() || !second.IsValid()) {
        return QueryResult::InvalidInput;
    }
    const T radiusSum = first.radius + second.radius;
    return detail::ToQueryResult(DistanceSquared(first.center, second.center) <=
                                 radiusSum * radiusSum);
}

// sphere 중심에서 box까지 제곱 거리 <= r²(Arvo 방식, UE SphereAABBIntersection과 같은 의미)다.
// 접촉 = Hit은 반올림 한계 안에서 성립한다: 축별 차이의 제곱 합과 r²를 각각 반올림하므로, 제곱 합이
// T의 가수 폭(double 53비트)을 넘는 큰 좌표에서는 정확히 표면에 닿는 box를 Miss로 볼 수 있다(정수
// 128비트 정확 판정과 대조한 무작위 시험에서 큰 좌표의 정확 접촉 사례 중 약 1.4%). 접촉을 반드시
// 잡아야 하면 호출자가 반지름에 여유를 더한다. 식은 box 좌표에 대해 단조라 더 큰 box의 거리는
// 작거나 같다.
template <typename T>
QueryResult Overlap(const Sphere<T>& sphere, const Box<T>& box) noexcept {
    if (!sphere.IsValid() || !box.IsValid()) {
        return QueryResult::InvalidInput;
    }
    const T distanceSquared = detail::BoxDistanceSquared(box.min, box.max, sphere.center);
    return detail::ToQueryResult(distanceSquared <= sphere.radius * sphere.radius);
}

template <typename T>
QueryResult Overlap(const Box<T>& box, const Sphere<T>& sphere) noexcept {
    return Overlap(sphere, box);
}

// 축별 구간 겹침이다. 면·모서리·꼭짓점 접촉과 0 부피 box도 겹침이다(UE TBox::Intersect와 같은 닫힌
// 판정).
template <typename T>
QueryResult Overlap(const Box<T>& first, const Box<T>& second) noexcept {
    if (!first.IsValid() || !second.IsValid()) {
        return QueryResult::InvalidInput;
    }
    const bool overlapX = first.min.x <= second.max.x && second.min.x <= first.max.x;
    const bool overlapY = first.min.y <= second.max.y && second.min.y <= first.max.y;
    const bool overlapZ = first.min.z <= second.max.z && second.min.z <= first.max.z;
    return detail::ToQueryResult(overlapX && overlapY && overlapZ);
}

template <typename T>
QueryResult Overlap(const Box<T>& box, const Plane<T>& plane) noexcept {
    const PlaneSide side = ClassifyPlaneSide(plane, box);
    if (side == PlaneSide::InvalidInput) {
        return QueryResult::InvalidInput;
    }
    return detail::ToQueryResult(side == PlaneSide::Intersecting);
}

template <typename T>
QueryResult Overlap(const Plane<T>& plane, const Box<T>& box) noexcept {
    return Overlap(box, plane);
}

template <typename T>
QueryResult Overlap(const Sphere<T>& sphere, const Plane<T>& plane) noexcept {
    const PlaneSide side = ClassifyPlaneSide(plane, sphere);
    if (side == PlaneSide::InvalidInput) {
        return QueryResult::InvalidInput;
    }
    return detail::ToQueryResult(side == PlaneSide::Intersecting);
}

template <typename T>
QueryResult Overlap(const Plane<T>& plane, const Sphere<T>& sphere) noexcept {
    return Overlap(sphere, plane);
}

// sphere 중심과 capsule 축 사이 제곱 거리 <= (r_capsule + r_sphere)²다.
template <typename T>
QueryResult Overlap(const Capsule<T>& capsule, const Sphere<T>& sphere) noexcept {
    if (!capsule.IsValid() || !sphere.IsValid()) {
        return QueryResult::InvalidInput;
    }
    const T radiusSum = capsule.radius + sphere.radius;
    const T distanceSquared =
        detail::SegmentDistanceSquared(sphere.center, capsule.start, capsule.end);
    return detail::ToQueryResult(distanceSquared <= radiusSum * radiusSum);
}

template <typename T>
QueryResult Overlap(const Sphere<T>& sphere, const Capsule<T>& capsule) noexcept {
    return Overlap(capsule, sphere);
}

// capsule이 평면에 닿거나 걸치면 Hit다(ClassifyPlaneSide가 Intersecting).
template <typename T>
QueryResult Overlap(const Capsule<T>& capsule, const Plane<T>& plane) noexcept {
    const PlaneSide side = ClassifyPlaneSide(plane, capsule);
    if (side == PlaneSide::InvalidInput) {
        return QueryResult::InvalidInput;
    }
    return detail::ToQueryResult(side == PlaneSide::Intersecting);
}

template <typename T>
QueryResult Overlap(const Plane<T>& plane, const Capsule<T>& capsule) noexcept {
    return Overlap(capsule, plane);
}

// cylinder가 평면에 닿거나 걸치면 Hit다(ClassifyPlaneSide가 Intersecting).
template <typename T>
QueryResult Overlap(const Cylinder<T>& cylinder, const Plane<T>& plane) noexcept {
    const PlaneSide side = ClassifyPlaneSide(plane, cylinder);
    if (side == PlaneSide::InvalidInput) {
        return QueryResult::InvalidInput;
    }
    return detail::ToQueryResult(side == PlaneSide::Intersecting);
}

template <typename T>
QueryResult Overlap(const Plane<T>& plane, const Cylinder<T>& cylinder) noexcept {
    return Overlap(cylinder, plane);
}

// sphere 중심에서 cylinder까지 제곱 거리 <= r_sphere²다. 거리는 축 방향(끝면 밖)과 수직
// 방향(반지름 밖) 초과분의 제곱합이다(TryDistanceSquared(cylinder)와 같은 식). 그래서 반지름 0
// sphere는 ContainsPoint(cylinder, center)와 같은 결과다. radius 0 cylinder는 선분 거리다. 접촉 =
// Hit은 반올림 한계 안에서 성립한다.
template <typename T>
QueryResult Overlap(const Cylinder<T>& cylinder, const Sphere<T>& sphere) noexcept {
    if (!cylinder.IsValid() || !sphere.IsValid()) {
        return QueryResult::InvalidInput;
    }
    const T distanceSquared = detail::CylinderDistanceSquared(cylinder, sphere.center);
    return detail::ToQueryResult(distanceSquared <= sphere.radius * sphere.radius);
}

template <typename T>
QueryResult Overlap(const Sphere<T>& sphere, const Cylinder<T>& cylinder) noexcept {
    return Overlap(cylinder, sphere);
}

// segment가 cylinder에 닿으면 Hit다(SegmentCast(segment, cylinder)가 Hit, 끝점 접촉 보정 포함).
template <typename T>
QueryResult Overlap(const Segment<T>& segment, const Cylinder<T>& cylinder) noexcept {
    const SegmentHit<T> hit = SegmentCast(segment, cylinder);
    return hit.result;
}

template <typename T>
QueryResult Overlap(const Cylinder<T>& cylinder, const Segment<T>& segment) noexcept {
    return Overlap(segment, cylinder);
}

// 두 축 segment의 최근접 제곱 거리 <= (r1 + r2)²다. 인자 순서와 무관하게 같은 순서로 계산한다.
template <typename T>
QueryResult Overlap(const Capsule<T>& first, const Capsule<T>& second) noexcept {
    if (!first.IsValid() || !second.IsValid()) {
        return QueryResult::InvalidInput;
    }
    const SegmentClosestPoints<T> closest =
        detail::SymmetricClosestSegmentSegment(first.Axis(), second.Axis());
    const T radiusSum = first.radius + second.radius;
    return detail::ToQueryResult(closest.distanceSquared <= radiusSum * radiusSum);
}

// 삼각형 위 최근접점까지 제곱 거리 <= r²다. 퇴화 삼각형은 선분·점으로 판정한다.
template <typename T>
QueryResult Overlap(const Sphere<T>& sphere, const Triangle<T>& triangle) noexcept {
    if (!sphere.IsValid() || !triangle.IsValid()) {
        return QueryResult::InvalidInput;
    }
    const Vector3<T> closest =
        detail::ClosestOnTriangle(sphere.center, triangle.a, triangle.b, triangle.c);
    const T distanceSquared = DistanceSquared(sphere.center, closest);
    return detail::ToQueryResult(distanceSquared <= sphere.radius * sphere.radius);
}

template <typename T>
QueryResult Overlap(const Triangle<T>& triangle, const Sphere<T>& sphere) noexcept {
    return Overlap(sphere, triangle);
}

// 15축 분리축 판정이다. parallelEpsilon(유한, 0 이상)은 회전 행렬 원소 절댓값에 더하는 보정값이며
// 판정을 겹침 쪽으로 약 parallelEpsilon * extent만큼 보수적으로 만든다. 0을 넘기면 보정 없이
// 계산하지만 평행 모서리에서 반올림 잡음으로 잘못 분리될 수 있다. 음수·NaN·Inf면 InvalidInput이다.
template <typename T>
QueryResult Overlap(const OrientedBox<T>& first, const OrientedBox<T>& second,
                    detail::GeometryScalar<T> parallelEpsilon = ObbParallelEpsilon<T>) noexcept {
    if (!first.IsValid() || !second.IsValid() || !detail::IsNonNegativeFinite(parallelEpsilon)) {
        return QueryResult::InvalidInput;
    }
    const bool swap = detail::OrientedBoxOrderedBefore(second, first);
    const OrientedBox<T>& left = swap ? second : first;
    const OrientedBox<T>& right = swap ? first : second;
    return detail::ToQueryResult(!detail::OrientedBoxesSeparated(left, right, parallelEpsilon));
}

// 볼록 영역과 box의 보수적 겹침이다(어느 평면에서도 엄격히 바깥이 아님). 모서리 근처의 초과 Hit는
// ClassifyContainment 설명을 따른다.
template <typename T>
QueryResult Overlap(const ConvexVolume<T>& volume, const Box<T>& box) noexcept {
    const Containment containment = ClassifyContainment(volume, box);
    if (containment == Containment::InvalidInput) {
        return QueryResult::InvalidInput;
    }
    return detail::ToQueryResult(containment != Containment::Outside);
}

template <typename T>
QueryResult Overlap(const Box<T>& box, const ConvexVolume<T>& volume) noexcept {
    return Overlap(volume, box);
}

// 볼록 영역과 sphere의 보수적 겹침이다(어느 평면에서도 중심이 반지름보다 더 뒤가 아님).
template <typename T>
QueryResult Overlap(const ConvexVolume<T>& volume, const Sphere<T>& sphere) noexcept {
    const Containment containment = ClassifyContainment(volume, sphere);
    if (containment == Containment::InvalidInput) {
        return QueryResult::InvalidInput;
    }
    return detail::ToQueryResult(containment != Containment::Outside);
}

template <typename T>
QueryResult Overlap(const Sphere<T>& sphere, const ConvexVolume<T>& volume) noexcept {
    return Overlap(volume, sphere);
}

// 볼록 영역과 capsule의 보수적 겹침이다(어느 평면에서도 엄격히 바깥이 아님).
template <typename T>
QueryResult Overlap(const ConvexVolume<T>& volume, const Capsule<T>& capsule) noexcept {
    const Containment containment = ClassifyContainment(volume, capsule);
    if (containment == Containment::InvalidInput) {
        return QueryResult::InvalidInput;
    }
    return detail::ToQueryResult(containment != Containment::Outside);
}

template <typename T>
QueryResult Overlap(const Capsule<T>& capsule, const ConvexVolume<T>& volume) noexcept {
    return Overlap(volume, capsule);
}

// 볼록 영역과 cylinder의 보수적 겹침이다(어느 평면에서도 엄격히 바깥이 아님).
template <typename T>
QueryResult Overlap(const ConvexVolume<T>& volume, const Cylinder<T>& cylinder) noexcept {
    const Containment containment = ClassifyContainment(volume, cylinder);
    if (containment == Containment::InvalidInput) {
        return QueryResult::InvalidInput;
    }
    return detail::ToQueryResult(containment != Containment::Outside);
}

template <typename T>
QueryResult Overlap(const Cylinder<T>& cylinder, const ConvexVolume<T>& volume) noexcept {
    return Overlap(volume, cylinder);
}

} // namespace rocks
