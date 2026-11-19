#pragma once

#include <Core/Math/IntersectionCylinderDetail.h>

namespace rocks {

// 점 포함(ContainsPoint), 평면 분류(ClassifyPlaneSide), 볼록 영역·포함 3분류
// (ClassifyContainment)다.

// ===== 점 포함 =====
// 경계 위의 점은 포함(Hit)이다. 유한하지 않은 point나 잘못된 shape는 InvalidInput이다.

template <typename T>
QueryResult ContainsPoint(const Box<T>& box, const Vector3<T>& point) noexcept {
    if (!box.IsValid() || !point.IsFinite()) {
        return QueryResult::InvalidInput;
    }
    return detail::ToQueryResult(detail::BoxContains(box.min, box.max, point));
}

template <typename T>
QueryResult ContainsPoint(const Sphere<T>& sphere, const Vector3<T>& point) noexcept {
    if (!sphere.IsValid() || !point.IsFinite()) {
        return QueryResult::InvalidInput;
    }
    const T distanceSquared = DistanceSquared(sphere.center, point);
    return detail::ToQueryResult(distanceSquared <= sphere.radius * sphere.radius);
}

template <typename T>
QueryResult ContainsPoint(const Capsule<T>& capsule, const Vector3<T>& point) noexcept {
    if (!capsule.IsValid() || !point.IsFinite()) {
        return QueryResult::InvalidInput;
    }
    const T distanceSquared = detail::SegmentDistanceSquared(point, capsule.start, capsule.end);
    return detail::ToQueryResult(distanceSquared <= capsule.radius * capsule.radius);
}

// local 좌표로 바꿔 비교한다. 항등 회전에서는 정확하지만 회전된 box의 경계 판정은 반올림 영향을
// 받는다.
template <typename T>
QueryResult ContainsPoint(const OrientedBox<T>& box, const Vector3<T>& point) noexcept {
    if (!box.IsValid() || !point.IsFinite()) {
        return QueryResult::InvalidInput;
    }
    const Vector3<T> local = box.ToLocal(point);
    return detail::ToQueryResult(detail::BoxContains(-box.halfExtents, box.halfExtents, local));
}

// t = (p - start)·a >= 0, (p - end)·a <= 0(두 끝면 사이)이고 수직 성분 v = (p - start) - t·a가
// |v|² <= r²이면 Hit다. p == start, p == end는 정확히 Hit다. 접촉 = Hit은 반올림 한계 안에서
// 성립한다(좌표축과 평행하지 않은 축은 정규화·내적 반올림으로 경계 위 점이 어느 쪽으로든 바뀔 수
// 있다). radius 0은 선분 위(최근접 거리 정확히 0)일 때만 Hit다(Capsule radius 0과 같다).
template <typename T>
QueryResult ContainsPoint(const Cylinder<T>& cylinder, const Vector3<T>& point) noexcept {
    if (!cylinder.IsValid() || !point.IsFinite()) {
        return QueryResult::InvalidInput;
    }
    if (!(cylinder.radius > static_cast<T>(0))) {
        return detail::ToQueryResult(detail::SegmentDistanceSquared(
                                         point, cylinder.start, cylinder.end) <= static_cast<T>(0));
    }
    const detail::CylinderCoordinates<T> coordinates =
        detail::ToCylinderCoordinates(cylinder, detail::CylinderUnitAxis(cylinder), point);
    return detail::ToQueryResult(detail::CylinderCoordinatesInside(coordinates, cylinder.radius));
}

// ===== 평면 분류 =====

// box의 평면 기준 위치다. 평면에 닿으면 Intersecting이다. UE PlaneAABBRelativePosition과 같은
// 꼭짓점 선택 방식이며 결과를 정수 대신 enum으로 돌려준다.
template <typename T>
PlaneSide ClassifyPlaneSide(const Plane<T>& plane, const Box<T>& box) noexcept {
    if (!plane.IsValid() || !box.IsValid()) {
        return PlaneSide::InvalidInput;
    }
    return detail::ClassifyBoxUnchecked(plane, box);
}

// |SignedDistance(center)| <= radius면 Intersecting이다.
template <typename T>
PlaneSide ClassifyPlaneSide(const Plane<T>& plane, const Sphere<T>& sphere) noexcept {
    if (!plane.IsValid() || !sphere.IsValid()) {
        return PlaneSide::InvalidInput;
    }
    return detail::ClassifySphereUnchecked(plane, sphere);
}

// capsule의 평면 기준 위치다. 두 끝점의 SignedDistance 중 작은 값 > r이면 Front, 큰 값 < -r이면
// Back, 그 밖(평면에 닿음 포함)은 Intersecting이다. support 반폭 (H/2)|n·a| + r(H = 0이면 r)을
// 중심 거리와 비교하는 것과 같은 값이며, start == end이면 ClassifyPlaneSide(plane, sphere)와 비트
// 단위로 같다. 평면 normal이 길이 오차(ShapeUnitTolerance) 안의 비단위 벡터이면 sphere와 같이
// 반지름의 약 1e-5 비율만큼 경계가 달라질 수 있다.
template <typename T>
PlaneSide ClassifyPlaneSide(const Plane<T>& plane, const Capsule<T>& capsule) noexcept {
    if (!plane.IsValid() || !capsule.IsValid()) {
        return PlaneSide::InvalidInput;
    }
    return detail::ClassifySweptUnchecked(plane, capsule.start, capsule.end, capsule.radius);
}

// cylinder의 평면 기준 위치다. 원판 support 반폭 h = r·|n × a|에 대해 두 끝면 중심의
// SignedDistance 중 작은 값 > h이면 Front, 큰 값 < -h이면 Back, 그 밖(평면에 닿음 포함)은
// Intersecting이다. 중심 거리와 support 반폭 (H/2)|n·a| + r|n × a|를 비교하는 것과 같은 값이다.
// |n × a|는 √(1 - (n·a)²) 꼴과 달리 평면 normal과 축이 거의 평행해도 소거가 없고, 비단위
// normal에도 정확한 support다. radius 0은 선분 분류(h = 0)다. 접촉 = Intersecting은 반올림 한계
// 안에서 성립한다(정규화한 축과 외적·내적 반올림).
template <typename T>
PlaneSide ClassifyPlaneSide(const Plane<T>& plane, const Cylinder<T>& cylinder) noexcept {
    if (!plane.IsValid() || !cylinder.IsValid()) {
        return PlaneSide::InvalidInput;
    }
    return detail::ClassifySweptUnchecked(plane, cylinder.start, cylinder.end,
                                          detail::CylinderDiscSupport(plane.normal, cylinder));
}

// ===== 볼록 영역·포함 3분류 =====
// 규칙은 IntersectionTypes.h의 Containment 설명을 따른다.

// box가 볼록 영역 밖·안·걸침인지다. 어느 평면에서든 가장 앞 꼭짓점이 엄격히 뒤(SignedDistance <
// 0)면 Outside, 모든 평면에서 가장 뒤 꼭짓점이 앞이거나 평면 위(>= 0)면 Inside다. 접촉 = 교차는
// 반올림 한계 안에서 성립한다: SignedDistance는 내적과 뺄셈을 반올림하므로 평면이 꼭짓점을 정확히
// 지날 때 그 꼭짓점의 부호가 양쪽 어느 쪽으로도 틀릴 수 있다(정수 정확 판정과 대조한 시험에서 확인,
// 접촉 box를 Outside로 보거나 바깥 box를 Intersecting으로 본다). 이 오차와 무관하게 식은 box 좌표에
// 대해 단조다. Overlap(volume, box)도 같다.
template <typename T>
Containment ClassifyContainment(const ConvexVolume<T>& volume, const Box<T>& box) noexcept {
    if (!volume.IsValid() || !box.IsValid()) {
        return Containment::InvalidInput;
    }
    return detail::ClassifyVolumeBoxUnchecked(volume, box);
}

// sphere가 볼록 영역 밖·안·걸침인지다. 어느 평면에서든 중심의 SignedDistance < −radius면 Outside,
// 모든 평면에서 SignedDistance >= radius면 Inside다.
template <typename T>
Containment ClassifyContainment(const ConvexVolume<T>& volume, const Sphere<T>& sphere) noexcept {
    if (!volume.IsValid() || !sphere.IsValid()) {
        return Containment::InvalidInput;
    }
    return detail::ClassifyVolumeSphereUnchecked(volume, sphere);
}

// box가 sphere 밖·안·걸침인지다. 중심에서 box까지 제곱 거리 > r²면 Outside(Overlap(sphere, box)의
// 부정과 같은 식), 가장 먼 꼭짓점까지 제곱 거리 <= r²면 Inside다.
template <typename T>
Containment ClassifyContainment(const Sphere<T>& sphere, const Box<T>& box) noexcept {
    if (!sphere.IsValid() || !box.IsValid()) {
        return Containment::InvalidInput;
    }
    return detail::ClassifySphereBoxUnchecked(sphere, box);
}

// capsule이 볼록 영역 밖·안·걸침인지다. 평면마다 두 축 끝점의 SignedDistance를 보고, 어느
// 평면에서든 큰 값 < -radius면 Outside, 모든 평면에서 작은 값 >= radius면 Inside다(밖은 엄격,
// 접촉은 교차, 안은 경계 포함). start == end이면 ClassifyContainment(volume, sphere)와 같다. 평면별
// 판정이라 모서리 근처에서 실제로 겹치지 않는 capsule도 Intersecting일 수 있다(Box와 같은 보수적
// 술어).
template <typename T>
Containment ClassifyContainment(const ConvexVolume<T>& volume, const Capsule<T>& capsule) noexcept {
    if (!volume.IsValid() || !capsule.IsValid()) {
        return Containment::InvalidInput;
    }
    const T radius = capsule.radius;
    return detail::ClassifyVolumeSweptUnchecked(
        volume, capsule.start, capsule.end, [radius](const Plane<T>&) noexcept { return radius; });
}

// cylinder가 볼록 영역 밖·안·걸침인지다. 평면마다 원판 support 반폭 h = r·|n × a|와 두 끝면 중심의
// SignedDistance를 보고, 어느 평면에서든 큰 값 < -h면 Outside, 모든 평면에서 작은 값 >= h면
// Inside다(밖은 엄격, 접촉은 교차, 안은 경계 포함 — Box·Sphere와 같은 규칙). 평면별 판정이라 모서리
// 근처의 초과 Intersecting은 Box와 같다. 접촉 정밀도 한계는 ClassifyPlaneSide(plane, cylinder)와
// 같다.
//
// 사용 예(프러스텀 컬링):
//   if (ClassifyContainment(frustum, pillar) != Containment::Outside) { Draw(pillar); }
template <typename T>
Containment ClassifyContainment(const ConvexVolume<T>& volume,
                                const Cylinder<T>& cylinder) noexcept {
    if (!volume.IsValid() || !cylinder.IsValid()) {
        return Containment::InvalidInput;
    }
    return detail::ClassifyVolumeSweptUnchecked(
        volume, cylinder.start, cylinder.end, [&cylinder](const Plane<T>& plane) noexcept {
            return detail::CylinderDiscSupport(plane.normal, cylinder);
        });
}

} // namespace rocks
