#pragma once

#include <Core/Math/IntersectionDetail.h>

namespace rocks {

// ray·segment cast의 shape별 내부 구현이다(구, box, 삼각형, capsule, OrientedBox).

namespace detail {

// ---- cast 공통 구현: origin + delta * t, t ∈ [0, tMax] ----
// 구 교차. 판별식 b² - a·c는 직선 위 중심 최근접점까지의 수직 벡터로 a·(r² - |수직|²)로 계산하고,
// 가까운 근은 c / (-b + sqrt(...))로 구해 큰 거리에서의 상쇄를 피한다(Ray Tracing Gems 7장 방식).
template <typename T>
CastResult<T> CastSphere(const Vector3<T>& origin, const Vector3<T>& delta, T tMax,
                         const Vector3<T>& center, T radius) noexcept {
    const T zero = static_cast<T>(0);
    const Vector3<T> offset = origin - center;
    const T radiusSquared = radius * radius;
    const T startGap = Dot(offset, offset) - radiusSquared;
    if (startGap <= zero) {
        return CastInside(origin);
    }

    const T a = Dot(delta, delta);
    const T b = Dot(offset, delta);
    if (a == zero || b >= zero) {
        // 0 길이이거나 중심에서 멀어지는(또는 수직으로 지나는) 중이다.
        return {};
    }

    const Vector3<T> perpendicular = offset - delta * (b / a);
    const T inner = radiusSquared - Dot(perpendicular, perpendicular);
    if (inner < zero) {
        return {};
    }

    const T t = startGap / (-b + math::Sqrt(a * inner));
    if (t > tMax) {
        return {};
    }

    const Vector3<T> point = origin + delta * t;
    const Vector3<T> normal = radius > zero ? (point - center) / radius : OpposingDirection(delta);
    return CastHit(t, point, normal);
}

// slab 판정이다. 시작점이 닫힌 box 안이면 내부 hit이다. 방향 성분이 정확히 0인 축은 slab 안일 때만
// 통과시킨다. 역수를 곱하지 않고 나누므로 subnormal 방향 성분에서도 0 * Inf = NaN이 생기지 않는다.
// hit point의 진입 축 좌표는 면 좌표로 고정하고 나머지 축은 반올림으로 box 밖에 나가지 않도록
// 자른다.
template <typename T>
CastResult<T> CastBox(const Vector3<T>& origin, const Vector3<T>& delta, T tMax,
                      const Vector3<T>& minimum, const Vector3<T>& maximum) noexcept {
    const T zero = static_cast<T>(0);
    if (BoxContains(minimum, maximum, origin)) {
        return CastInside(origin);
    }

    const T start[3] = {origin.x, origin.y, origin.z};
    const T step[3] = {delta.x, delta.y, delta.z};
    const T low[3] = {minimum.x, minimum.y, minimum.z};
    const T high[3] = {maximum.x, maximum.y, maximum.z};

    T enter = zero;
    T leave = tMax;
    int enterAxis = -1;
    bool enterHighFace = false;
    for (int axis = 0; axis < 3; ++axis) {
        const bool below = start[axis] < low[axis];
        const bool above = start[axis] > high[axis];
        if (step[axis] == zero) {
            if (below || above) {
                return {};
            }
            continue;
        }

        const bool positive = step[axis] > zero;
        if ((below && !positive) || (above && positive)) {
            return {};
        }

        const T toLow = (low[axis] - start[axis]) / step[axis];
        const T toHigh = (high[axis] - start[axis]) / step[axis];
        const T slabEnter = positive ? toLow : toHigh;
        const T slabLeave = positive ? toHigh : toLow;

        // slab 밖에서 출발한 축만 진입 면 후보다. slab 안의 축은 진입 시각이 0 이하다.
        if ((below || above) && (enterAxis < 0 || slabEnter > enter)) {
            enter = slabEnter;
            enterAxis = axis;
            enterHighFace = !positive;
        }
        if (slabLeave < leave) {
            leave = slabLeave;
        }
    }

    // 시작점이 밖이면 slab 밖 축이 있고 그 축의 방향 성분은 0이 아니므로 enterAxis가 정해진다.
    if (enterAxis < 0 || enter > leave) {
        return {};
    }

    T hitPoint[3];
    T hitNormal[3] = {zero, zero, zero};
    for (int axis = 0; axis < 3; ++axis) {
        hitPoint[axis] = ClampComponent(start[axis] + step[axis] * enter, low[axis], high[axis]);
    }
    hitPoint[enterAxis] = enterHighFace ? high[enterAxis] : low[enterAxis];
    hitNormal[enterAxis] = enterHighFace ? static_cast<T>(1) : static_cast<T>(-1);
    return CastHit(enter, Vector3<T>(hitPoint[0], hitPoint[1], hitPoint[2]),
                   Vector3<T>(hitNormal[0], hitNormal[1], hitNormal[2]));
}

// Möller–Trumbore 판정이다. 무게중심 좌표를 det로 나누기 전에 [0, det] 범위로 비교하여 나눗셈
// 반올림이 경계 판정을 바꾸지 않게 한다. det = -Dot(delta, 앞면 normal)이므로 det > 0이면 앞면에서
// 들어온다. 경계(변·꼭짓점) 위 hit은 포함하지만 반올림 때문에 변을 공유하는 두 삼각형이 모두 Miss일
// 수 있다 (watertight 판정은 보장하지 않는다). origin이 삼각형 위(정확 판정)면 방향과 무관하게 거리
// 0 Hit다(FrontOnly는 앞면 진입일 때만). 그 밖의 퇴화 삼각형과 det == 0(평행·공면)은 Miss다. det가
// 정확히 0이 되지 않는 회전된 공면 ray는 반올림에 따라 Hit일 수 있다.
template <typename T>
CastResult<T> CastTriangle(const Vector3<T>& origin, const Vector3<T>& delta, T tMax,
                           const Triangle<T>& triangle, TriangleSides sides) noexcept {
    const T zero = static_cast<T>(0);
    const Vector3<T> edge1 = triangle.b - triangle.a;
    const Vector3<T> edge2 = triangle.c - triangle.a;
    const Vector3<T> faceNormal = Cross(edge1, edge2);
    if (faceNormal == Vector3<T>::Zero()) {
        return {};
    }

    const Vector3<T> p = Cross(delta, edge2);
    T det = Dot(edge1, p);
    const bool frontFace = det > zero;

    // 시작 접촉: origin이 삼각형 위(평면 위 정확 판정, 경계 포함)면 닫힌 집합 규칙대로 방향과
    // 무관하게 거리 0 Hit다(평면 시작과 같다). 회전된 삼각형에서는 아래 scaledT가 반올림으로 작은
    // 음수가 되어 이 접촉을 놓칠 수 있으므로 먼저 판정한다. 평면 판정이 0이 아니면 추가 비용은 내적
    // 하나다.
    if (Dot(faceNormal, origin - triangle.a) == zero && PointOnTriangle(triangle, origin)) {
        if (sides == TriangleSides::FrontOnly && !frontFace) {
            return {};
        }
        auto unitNormal = faceNormal.TryNormalize();
        if (!unitNormal) {
            return {};
        }
        // 공면(det == 0)이면 앞면 normal 그대로다.
        const Vector3<T> normal = det < zero ? -unitNormal.Value() : unitNormal.Value();
        return CastHit(zero, origin, normal);
    }

    if (det == zero) {
        return {};
    }
    if (sides == TriangleSides::FrontOnly && !frontFace) {
        return {};
    }

    const Vector3<T> s = origin - triangle.a;
    const Vector3<T> q = Cross(s, edge1);
    T u = Dot(s, p);
    T v = Dot(delta, q);
    T scaledT = Dot(edge2, q);
    if (!frontFace) {
        det = -det;
        u = -u;
        v = -v;
        scaledT = -scaledT;
    }
    if (u < zero || u > det || v < zero || u + v > det || scaledT < zero) {
        return {};
    }

    // 뒷면 부호 반전으로 scaledT가 -0이 될 수 있다. 시작 접촉 거리는 +0으로 낸다.
    const T t = scaledT == zero ? zero : scaledT / det;
    if (t > tMax) {
        return {};
    }

    auto unitNormal = faceNormal.TryNormalize();
    if (!unitNormal) {
        return {};
    }
    const Vector3<T> normal = frontFace ? unitNormal.Value() : -unitNormal.Value();
    return CastHit(t, origin + delta * t, normal);
}

// capsule = 축 구간의 원기둥 옆면 ∪ 양 끝 구. 시작점이 밖이면 첫 진입은 (축 범위 안의 옆면 진입, 두
// 끝 구의 진입) 중 가장 이른 것이다. 원기둥의 평평한 끝면은 끝 구 안에 있으므로 따로 검사하지
// 않는다.
template <typename T>
CastResult<T> CastCapsule(const Vector3<T>& origin, const Vector3<T>& delta, T tMax,
                          const Capsule<T>& capsule) noexcept {
    const T zero = static_cast<T>(0);
    const T radiusSquared = capsule.radius * capsule.radius;
    if (SegmentDistanceSquared(origin, capsule.start, capsule.end) <= radiusSquared) {
        return CastInside(origin);
    }

    CastResult<T> best = CastSphere(origin, delta, tMax, capsule.start, capsule.radius);
    const CastResult<T> endCap = CastSphere(origin, delta, tMax, capsule.end, capsule.radius);
    if (endCap.result == QueryResult::Hit &&
        (best.result != QueryResult::Hit || endCap.t < best.t)) {
        best = endCap;
    }

    const Vector3<T> axis = capsule.end - capsule.start;
    const T axisLengthSquared = Dot(axis, axis);
    if (!(axisLengthSquared > zero)) {
        return best;
    }

    // 축 방향 성분을 뺀 수직 성분으로 무한 원기둥 |offset⊥ + delta⊥ t| = r을 구와 같은 방식으로
    // 푼다.
    const Vector3<T> offset = origin - capsule.start;
    const Vector3<T> offsetPerpendicular = offset - axis * (Dot(offset, axis) / axisLengthSquared);
    const Vector3<T> deltaPerpendicular = delta - axis * (Dot(delta, axis) / axisLengthSquared);
    const T a = Dot(deltaPerpendicular, deltaPerpendicular);
    const T b = Dot(offsetPerpendicular, deltaPerpendicular);
    const T startGap = Dot(offsetPerpendicular, offsetPerpendicular) - radiusSquared;
    if (startGap <= zero) {
        // 무한 원기둥 안(표면 포함)에서 출발한다. 축 범위 안이면 capsule 안이다. 위의 선분 거리
        // 판정과 이 수직 성분 판정은 반올림이 달라 옆면 위의 점에서 결과가 엇갈릴 수 있으므로
        // 여기서도 시작 접촉으로 처리한다. 축 범위 밖이면 끝 구가 첫 진입이다.
        const T alongStart = Dot(offset, axis);
        if (alongStart >= zero && alongStart <= axisLengthSquared) {
            return CastInside(origin);
        }
        return best;
    }
    if (!(a > zero) || b >= zero) {
        // 축과 평행하거나 축에서 멀어지면 옆면 진입이 없고 끝 구가 첫 진입이다.
        return best;
    }

    const Vector3<T> closest = offsetPerpendicular - deltaPerpendicular * (b / a);
    const T inner = radiusSquared - Dot(closest, closest);
    if (inner < zero) {
        return best;
    }

    const T t = startGap / (-b + math::Sqrt(a * inner));
    const Vector3<T> point = origin + delta * t;
    const T along = Dot(point - capsule.start, axis);
    const bool withinAxis = along >= zero && along <= axisLengthSquared;
    if (t > tMax || !withinAxis || (best.result == QueryResult::Hit && !(t < best.t))) {
        return best;
    }

    const Vector3<T> axisPoint = capsule.start + axis * (along / axisLengthSquared);
    const Vector3<T> normal =
        capsule.radius > zero ? (point - axisPoint) / capsule.radius : OpposingDirection(delta);
    return CastHit(t, point, normal);
}

// box local 좌표로 바꿔 slab 판정을 한다. 정규화한 회전은 길이를 보존하므로 t는 world 거리와 같다.
template <typename T>
CastResult<T> CastOrientedBox(const Vector3<T>& origin, const Vector3<T>& delta, T tMax,
                              const OrientedBox<T>& box) noexcept {
    const Quaternion<T> rotation = box.UnitRotation();
    const Vector3<T> localOrigin = rotation.UnrotateVector(origin - box.center);
    const Vector3<T> localDelta = rotation.UnrotateVector(delta);
    CastResult<T> cast = CastBox(localOrigin, localDelta, tMax, -box.halfExtents, box.halfExtents);
    if (cast.result != QueryResult::Hit) {
        return cast;
    }
    if (cast.startedInside) {
        return CastInside(origin);
    }

    cast.point = box.center + rotation.RotateVector(cast.point);
    cast.normal = rotation.RotateVector(cast.normal);
    return cast;
}

} // namespace detail

} // namespace rocks
