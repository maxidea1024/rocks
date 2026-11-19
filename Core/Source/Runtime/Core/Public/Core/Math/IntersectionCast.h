#pragma once

#include <Core/Math/IntersectionCylinderDetail.h>

namespace rocks {

// Ray 질의(Raycast)와 Segment 질의(SegmentCast)다. 계약은 IntersectionTypes.h를 따른다.

// ===== Ray 질의 =====
// 모든 Raycast는 ray.IsValid(), shape.IsValid(), maxDistance >= 0(NaN 제외, +Inf 허용)을 요구하며
// 위반이면 InvalidInput이다. 접촉 거리는 [0, maxDistance]다.
// 주의: maxDistance는 T로 변환된 뒤 검사한다(추론 제외 인자). float shape에 double 값을 넘기면
// float으로 반올림되므로 -1e-50 같은 아주 작은 음수는 -0(= 0으로 유효), float 범위를 넘는 값은
// +Inf가 된다. 음수 판정이 중요하면 호출 쪽에서 T로 바꾸기 전에 검사한다.

// 평면은 두께 없는 면이다. origin이 평면 위(SignedDistance == 0 정확)면 방향과 무관하게 거리 0
// Hit다. 평행한 ray는 origin이 평면 위가 아니면 Miss다. 뒤쪽 교차(거리 < 0)는 Miss다. normal은
// ray가 출발한 쪽(Dot(normal, direction) <= 0)을 향하며, 평면 위에서 평면과 평행하게 출발하면
// plane.normal 그대로다. UE FMath::RayPlaneIntersection은 평행 판정 없이 0으로 나누므로 Inf/NaN
// 점을 돌려줄 수 있다.
template <typename T>
RayHit<T> Raycast(const Ray<T>& ray, const Plane<T>& plane,
                  detail::GeometryScalar<T> maxDistance = detail::GeometryInfinity<T>()) noexcept {
    if (!ray.IsValid() || !plane.IsValid() || !detail::IsValidMaxDistance(maxDistance)) {
        return detail::InvalidRayHit<T>();
    }

    const T zero = static_cast<T>(0);
    const T startDistance = plane.SignedDistance(ray.origin);
    const T rate = Dot(plane.normal, ray.direction);
    if (startDistance == zero) {
        const Vector3<T> normal = rate > zero ? -plane.normal : plane.normal;
        return detail::ToRayHit(detail::CastHit(zero, ray.origin, normal));
    }
    if (rate == zero) {
        return {};
    }

    // 거의 평행한 ray는 나눗셈이 overflow해 distance가 +Inf가 될 수 있다. 유한한 교차점이 없으므로
    // maxDistance가 +Inf여도 Miss다(Inf 거리·NaN 점을 Hit로 내지 않는다).
    const T distance = -startDistance / rate;
    if (!math::IsFinite(distance) || distance < zero || distance > maxDistance) {
        return {};
    }
    const Vector3<T> point = ray.PointAt(distance);
    if (!point.IsFinite()) {
        return {};
    }

    const Vector3<T> normal = startDistance > zero ? plane.normal : -plane.normal;
    return detail::ToRayHit(detail::CastHit(distance, point, normal));
}

// 반지름 0 sphere는 점이다. 점을 정확히 지나는 ray만 Hit이며 normal은 -direction이다.
template <typename T>
RayHit<T> Raycast(const Ray<T>& ray, const Sphere<T>& sphere,
                  detail::GeometryScalar<T> maxDistance = detail::GeometryInfinity<T>()) noexcept {
    if (!ray.IsValid() || !sphere.IsValid() || !detail::IsValidMaxDistance(maxDistance)) {
        return detail::InvalidRayHit<T>();
    }
    return detail::ToRayHit(
        detail::CastSphere(ray.origin, ray.direction, maxDistance, sphere.center, sphere.radius));
}

// slab 판정이다. 면에 평행하게 면 위를 스치는 ray도 Hit다. point의 진입 축 좌표는 면 좌표와 정확히
// 같다. UE LineBoxIntersection의 0.1 단위 측면 허용 오차(BOX_SIDE_THRESHOLD) 같은 숨은 여유는 없다.
// 접촉 = Hit은 반올림 한계 안에서 성립한다: slab 진입·이탈 시각을 나눗셈으로 구하므로 정확한
// 판정과 다를 수 있는데, 정수 정확 판정과 대조한 시험에서 다른 경우는 모두 Hit로 넓히는 쪽이었다
// (실제로 닿지 않는 거의 접선인 ray가 Hit). 진입 거리는 box 좌표에 대해 단조라 더 큰 box의 진입
// 거리는 작거나 같다.
template <typename T>
RayHit<T> Raycast(const Ray<T>& ray, const Box<T>& box,
                  detail::GeometryScalar<T> maxDistance = detail::GeometryInfinity<T>()) noexcept {
    if (!ray.IsValid() || !box.IsValid() || !detail::IsValidMaxDistance(maxDistance)) {
        return detail::InvalidRayHit<T>();
    }
    return detail::ToRayHit(
        detail::CastBox(ray.origin, ray.direction, maxDistance, box.min, box.max));
}

// 기본은 양면 판정이며 normal은 ray가 온 쪽을 향한다. 퇴화 삼각형과 평행·공면 ray(det == 0)는
// Miss다. 경계 포함·watertight 한계는 detail::CastTriangle 주석을 따른다.
template <typename T>
RayHit<T> Raycast(const Ray<T>& ray, const Triangle<T>& triangle,
                  detail::GeometryScalar<T> maxDistance = detail::GeometryInfinity<T>(),
                  TriangleSides sides = TriangleSides::TwoSided) noexcept {
    if (!ray.IsValid() || !triangle.IsValid() || !detail::IsValidMaxDistance(maxDistance)) {
        return detail::InvalidRayHit<T>();
    }
    return detail::ToRayHit(
        detail::CastTriangle(ray.origin, ray.direction, maxDistance, triangle, sides));
}

template <typename T>
RayHit<T> Raycast(const Ray<T>& ray, const Capsule<T>& capsule,
                  detail::GeometryScalar<T> maxDistance = detail::GeometryInfinity<T>()) noexcept {
    if (!ray.IsValid() || !capsule.IsValid() || !detail::IsValidMaxDistance(maxDistance)) {
        return detail::InvalidRayHit<T>();
    }
    return detail::ToRayHit(detail::CastCapsule(ray.origin, ray.direction, maxDistance, capsule));
}

// local 좌표 slab 판정이다. 회전 반올림 때문에 경계 접촉은 정확하지 않다.
template <typename T>
RayHit<T> Raycast(const Ray<T>& ray, const OrientedBox<T>& box,
                  detail::GeometryScalar<T> maxDistance = detail::GeometryInfinity<T>()) noexcept {
    if (!ray.IsValid() || !box.IsValid() || !detail::IsValidMaxDistance(maxDistance)) {
        return detail::InvalidRayHit<T>();
    }
    return detail::ToRayHit(detail::CastOrientedBox(ray.origin, ray.direction, maxDistance, box));
}

// cylinder ray 판정이다. 축 slab 구간과 옆면 2차식 구간의 교집합으로 계산한다.
// - 축과 평행한 ray와 끝면 평면 안을 스치는 ray도 닫힌 집합이므로 Hit다.
// - 시작점이 안(경계 포함)이면 거리 0, startedInside = true, normal 영벡터다. 시작점 판정은
//   ContainsPoint와 같은 식이라 startedInside ⇔ ContainsPoint Hit다. 밖에서 출발했는데 진입 거리가
//   반올림·underflow로 0이 되면 거리 0, 진입 경계 normal, startedInside = false인 Hit다.
// - normal은 진입한 경계의 바깥 방향이다(끝면은 ±축, 옆면은 축에서 hit point로 향하는 수직
//   방향). 끝면과 옆면 진입 거리가 같은 모서리는 끝면 normal이다. 접선에 가까운 옆면 진입의
//   normal은 반올림으로 direction과 아주 조금 같은 쪽(내적이 0보다 약간 큼)일 수 있는 근사다.
// - radius 0(선분)은 Capsule radius 0과 같다: 선분을 정확히 지나는 ray만 Hit이고 normal은
//   -direction이다. 시작 접촉은 ContainsPoint(radius 0)와 같은 식으로만 정한다.
// 접촉 = Hit은 반올림 한계 안에서 성립한다: 축 정규화·내적·2차식 근을 T로 반올림하므로 경계를
// 정확히 스치는 ray가 어느 쪽으로든 바뀔 수 있다(좌표축과 평행한 축의 끝면·옆면 접선은 정확하다).
//
// 사용 예:
//   const RayHit<double> hit = Raycast(ray, pillar, 1000.0);
//   if (hit.IsHit() && !hit.startedInside) { Use(hit.point, hit.normal); }
template <typename T>
RayHit<T> Raycast(const Ray<T>& ray, const Cylinder<T>& cylinder,
                  detail::GeometryScalar<T> maxDistance = detail::GeometryInfinity<T>()) noexcept {
    if (!ray.IsValid() || !cylinder.IsValid() || !detail::IsValidMaxDistance(maxDistance)) {
        return detail::InvalidRayHit<T>();
    }
    return detail::ToRayHit(detail::CastCylinder(ray.origin, ray.direction, maxDistance, cylinder));
}

// ===== Segment 질의 =====
// segment.IsValid()와 shape.IsValid()를 요구한다. t ∈ [0,1]이며 끝점 접촉(t = 1)도 Hit다.

// 끝점의 SignedDistance d0, d1로 판정한다. d0 == 0이면 t = 0, d1 == 0이면 t = 1을 정확히 낸다. 평면
// 위에 놓인 segment와 평면 위의 0 길이 segment는 t = 0 Hit다. normal 규약은 Raycast(Plane)와 같다.
// UE SegmentPlaneIntersection은 매개변수에 ±1e-4(KINDA_SMALL_NUMBER) 여유를 두고 평면 위 segment를
// 0/0으로 놓친다.
template <typename T>
SegmentHit<T> SegmentCast(const Segment<T>& segment, const Plane<T>& plane) noexcept {
    if (!segment.IsValid() || !plane.IsValid()) {
        return detail::InvalidSegmentHit<T>();
    }

    const T zero = static_cast<T>(0);
    const T startDistance = plane.SignedDistance(segment.start);
    const T endDistance = plane.SignedDistance(segment.end);
    if (startDistance == zero) {
        const Vector3<T> normal = endDistance > zero ? -plane.normal : plane.normal;
        return detail::ToSegmentHit(detail::CastHit(zero, segment.start, normal));
    }

    const Vector3<T> normal = startDistance > zero ? plane.normal : -plane.normal;
    if (endDistance == zero) {
        return detail::ToSegmentHit(detail::CastHit(static_cast<T>(1), segment.end, normal));
    }
    if ((startDistance > zero) == (endDistance > zero)) {
        return {};
    }

    const T t = detail::ClampUnit(startDistance / (startDistance - endDistance));
    return detail::ToSegmentHit(detail::CastHit(t, segment.PointAt(t), normal));
}

// end가 닫힌 공 안인데 반올림으로 계산된 t가 1을 넘으면 t = 1 Hit로 보정한다(끝점 접촉 보존).
template <typename T>
SegmentHit<T> SegmentCast(const Segment<T>& segment, const Sphere<T>& sphere) noexcept {
    if (!segment.IsValid() || !sphere.IsValid()) {
        return detail::InvalidSegmentHit<T>();
    }

    const T one = static_cast<T>(1);
    const Vector3<T> delta = segment.Delta();
    const detail::CastResult<T> cast =
        detail::CastSphere(segment.start, delta, one, sphere.center, sphere.radius);
    if (cast.result == QueryResult::Hit) {
        return detail::ToSegmentHit(cast);
    }

    const Vector3<T> endOffset = segment.end - sphere.center;
    if (Dot(endOffset, endOffset) > sphere.radius * sphere.radius) {
        return {};
    }

    const Vector3<T> normal = sphere.radius > static_cast<T>(0) ? endOffset / sphere.radius
                                                                : detail::OpposingDirection(delta);
    return detail::ToSegmentHit(detail::CastHit(one, segment.end, normal));
}

// 0 길이 segment는 모든 축 방향 성분이 0이므로 점 포함 판정과 같다.
template <typename T>
SegmentHit<T> SegmentCast(const Segment<T>& segment, const Box<T>& box) noexcept {
    if (!segment.IsValid() || !box.IsValid()) {
        return detail::InvalidSegmentHit<T>();
    }
    return detail::ToSegmentHit(
        detail::CastBox(segment.start, segment.Delta(), static_cast<T>(1), box.min, box.max));
}

// start가 삼각형 위(평면 위 정확 판정, 경계 포함)면 방향과 무관하게 t = 0 Hit다(0 길이·공면
// segment 포함, normal은 Raycast 규칙). end가 삼각형 위(같은 정확 판정)인데 반올림한 t가 1을 넘어
// 놓치면 t = 1 Hit로 보정한다(끝점 접촉 보존). start가 삼각형 밖인 공면 segment는 Raycast와 같이
// Miss다. UE SegmentTriangleIntersection과
// 달리 평면 매개변수에 숨은 허용 오차가 없다.
template <typename T>
SegmentHit<T> SegmentCast(const Segment<T>& segment, const Triangle<T>& triangle,
                          TriangleSides sides = TriangleSides::TwoSided) noexcept {
    if (!segment.IsValid() || !triangle.IsValid()) {
        return detail::InvalidSegmentHit<T>();
    }

    const Vector3<T> delta = segment.Delta();
    if (delta == Vector3<T>::Zero()) {
        if (!detail::PointOnTriangle(triangle, segment.start)) {
            return {};
        }
        auto normal = triangle.TryNormal();
        if (!normal) {
            return {};
        }
        return detail::ToSegmentHit(
            detail::CastHit(static_cast<T>(0), segment.start, normal.Value()));
    }

    const T one = static_cast<T>(1);
    const detail::CastResult<T> cast =
        detail::CastTriangle(segment.start, delta, one, triangle, sides);
    if (cast.result == QueryResult::Hit || !detail::PointOnTriangle(triangle, segment.end)) {
        return detail::ToSegmentHit(cast);
    }

    // end는 평면 위(정확 판정)이므로 들어오는 면은 start가 있는 쪽이다. 회전된 삼각형에서는
    // Dot(delta, faceNormal)이 반올림으로 0이 아닐 수 있어 같은 정확 판정으로 start 쪽을 구한다.
    // start도 평면 위면 공면 segment이며 start가 삼각형 밖이므로(안이면 위에서 t = 0 Hit) Miss다.
    const T zero = static_cast<T>(0);
    const Vector3<T> faceNormal = Cross(triangle.b - triangle.a, triangle.c - triangle.a);
    const T startSide = Dot(faceNormal, segment.start - triangle.a);
    if (startSide == zero || (sides == TriangleSides::FrontOnly && startSide < zero)) {
        return {};
    }
    auto unitNormal = faceNormal.TryNormalize();
    if (!unitNormal) {
        return {};
    }
    const Vector3<T> normal = startSide > zero ? unitNormal.Value() : -unitNormal.Value();
    return detail::ToSegmentHit(detail::CastHit(one, segment.end, normal));
}

// Raycast(cylinder)와 같은 구간 교집합을 segment 매개변수 t ∈ [0, 1]로 계산한다. start가 안이면
// t = 0 시작 접촉이고 0 길이 segment는 점 포함 판정과 같다. end가 안(ContainsPoint와 같은 식)인데
// 반올림으로 계산된 진입 t가 1을 넘어 놓치면 t = 1 Hit로 보정한다(끝점 접촉 보존). 이때 normal은
// end에서 가장 가까운 경계(끝면·옆면, 같으면 끝면)의 바깥 방향이다. radius 0은 선분과 선분이 정확히
// 만날 때만 Hit이며 normal은 진행 방향의 반대다. 접촉 정밀도 한계는 Raycast(cylinder)와 같다.
template <typename T>
SegmentHit<T> SegmentCast(const Segment<T>& segment, const Cylinder<T>& cylinder) noexcept {
    if (!segment.IsValid() || !cylinder.IsValid()) {
        return detail::InvalidSegmentHit<T>();
    }

    const T one = static_cast<T>(1);
    const Vector3<T> delta = segment.Delta();
    const detail::CastResult<T> cast = detail::CastCylinder(segment.start, delta, one, cylinder);
    if (cast.result == QueryResult::Hit) {
        return detail::ToSegmentHit(cast);
    }

    if (!(cylinder.radius > static_cast<T>(0))) {
        if (!detail::CylinderContainsPoint(cylinder, segment.end)) {
            return {};
        }
        return detail::ToSegmentHit(
            detail::CastHit(one, segment.end, detail::OpposingDirection(delta)));
    }

    const Vector3<T> axis = detail::CylinderUnitAxis(cylinder);
    const detail::CylinderCoordinates<T> atEnd =
        detail::ToCylinderCoordinates(cylinder, axis, segment.end);
    if (!detail::CylinderCoordinatesInside(atEnd, cylinder.radius)) {
        return {};
    }
    const Vector3<T> normal = detail::CylinderNearestBoundaryNormal(cylinder, axis, atEnd, delta);
    return detail::ToSegmentHit(detail::CastHit(one, segment.end, normal));
}

} // namespace rocks
