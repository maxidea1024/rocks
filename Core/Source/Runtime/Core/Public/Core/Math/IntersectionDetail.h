#pragma once

#include <Core/Math/IntersectionTypes.h>

namespace rocks {

// 교차 질의의 공통 내부 구현이다: cast 결과 변환, 점–box·segment·삼각형 최근접, segment 쌍,
// 평면 분류 보조, OrientedBox 분리축, 볼록 영역 분류 보조.

namespace detail {

// ray·segment 공통 내부 결과다. t는 ray에서는 거리, segment에서는 [0,1] 매개변수다.
template <typename T>
struct CastResult {
    QueryResult result = QueryResult::Miss;
    T t = static_cast<T>(0);
    Vector3<T> point;
    Vector3<T> normal;
    bool startedInside = false;
};

template <typename T>
constexpr CastResult<T> CastInside(const Vector3<T>& origin) noexcept {
    CastResult<T> cast;
    cast.result = QueryResult::Hit;
    cast.point = origin;
    cast.startedInside = true;
    return cast;
}

template <typename T>
constexpr CastResult<T> CastHit(T t, const Vector3<T>& point, const Vector3<T>& normal) noexcept {
    CastResult<T> cast;
    cast.result = QueryResult::Hit;
    cast.t = t;
    cast.point = point;
    cast.normal = normal;
    return cast;
}

template <typename T>
constexpr RayHit<T> ToRayHit(const CastResult<T>& cast) noexcept {
    RayHit<T> hit;
    hit.result = cast.result;
    hit.distance = cast.t;
    hit.point = cast.point;
    hit.normal = cast.normal;
    hit.startedInside = cast.startedInside;
    return hit;
}

template <typename T>
constexpr SegmentHit<T> ToSegmentHit(const CastResult<T>& cast) noexcept {
    SegmentHit<T> hit;
    hit.result = cast.result;
    hit.t = cast.t;
    hit.point = cast.point;
    hit.normal = cast.normal;
    hit.startedInside = cast.startedInside;
    return hit;
}

template <typename T>
constexpr RayHit<T> InvalidRayHit() noexcept {
    RayHit<T> hit;
    hit.result = QueryResult::InvalidInput;
    return hit;
}

template <typename T>
constexpr SegmentHit<T> InvalidSegmentHit() noexcept {
    SegmentHit<T> hit;
    hit.result = QueryResult::InvalidInput;
    return hit;
}

// NaN과 음수는 false, +Inf와 -0은 true다.
template <typename T>
constexpr bool IsValidMaxDistance(T maxDistance) noexcept {
    return maxDistance >= static_cast<T>(0);
}

constexpr QueryResult ToQueryResult(bool hit) noexcept {
    return hit ? QueryResult::Hit : QueryResult::Miss;
}

template <typename T>
constexpr T ClampComponent(T value, T low, T high) noexcept {
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

template <typename T>
constexpr T ClampUnit(T value) noexcept {
    return ClampComponent(value, static_cast<T>(0), static_cast<T>(1));
}

// 진행 방향 delta(영벡터 아님)의 반대 방향 단위 벡터다. 반지름 0 shape hit의 normal로 쓴다.
template <typename T>
Vector3<T> OpposingDirection(const Vector3<T>& delta) noexcept {
    return -delta.NormalizedOr(Vector3<T>::Zero());
}

// ---- 점과 box ----
template <typename T>
constexpr bool BoxContains(const Vector3<T>& minimum, const Vector3<T>& maximum,
                           const Vector3<T>& point) noexcept {
    return point.x >= minimum.x && point.x <= maximum.x && point.y >= minimum.y &&
           point.y <= maximum.y && point.z >= minimum.z && point.z <= maximum.z;
}

// box 안의 성분은 그대로 두므로 내부 점은 정확히 자기 자신이다.
template <typename T>
constexpr Vector3<T> ClampToBox(const Vector3<T>& minimum, const Vector3<T>& maximum,
                                const Vector3<T>& point) noexcept {
    return Vector3<T>(ClampComponent(point.x, minimum.x, maximum.x),
                      ClampComponent(point.y, minimum.y, maximum.y),
                      ClampComponent(point.z, minimum.z, maximum.z));
}

template <typename T>
constexpr T BoxDistanceSquared(const Vector3<T>& minimum, const Vector3<T>& maximum,
                               const Vector3<T>& point) noexcept {
    return (point - ClampToBox(minimum, maximum, point)).LengthSquared();
}

// ---- 점과 segment ----
// 0 길이 segment는 start를 돌려준다. 투영이 끝점 밖이면 해당 끝점을 정확히 돌려준다.
template <typename T>
constexpr Vector3<T> ClosestOnSegment(const Vector3<T>& point, const Vector3<T>& start,
                                      const Vector3<T>& end) noexcept {
    const Vector3<T> delta = end - start;
    const T along = Dot(point - start, delta);
    if (along <= static_cast<T>(0)) {
        return start;
    }

    const T lengthSquared = Dot(delta, delta);
    if (lengthSquared <= along) {
        return end;
    }
    return Lerp(start, end, along / lengthSquared);
}

template <typename T>
constexpr T SegmentDistanceSquared(const Vector3<T>& point, const Vector3<T>& start,
                                   const Vector3<T>& end) noexcept {
    return DistanceSquared(point, ClosestOnSegment(point, start, end));
}

// ---- 점과 삼각형 ----
// 세 변에 대한 최근접점 중 가장 가까운 점이다. 퇴화 삼각형과 면 영역 안전장치에 쓴다.
template <typename T>
constexpr Vector3<T> ClosestOnTriangleEdges(const Vector3<T>& point, const Vector3<T>& a,
                                            const Vector3<T>& b, const Vector3<T>& c) noexcept {
    Vector3<T> best = ClosestOnSegment(point, a, b);
    T bestDistance = DistanceSquared(point, best);

    const Vector3<T> onBc = ClosestOnSegment(point, b, c);
    const T bcDistance = DistanceSquared(point, onBc);
    if (bcDistance < bestDistance) {
        best = onBc;
        bestDistance = bcDistance;
    }

    const Vector3<T> onCa = ClosestOnSegment(point, c, a);
    if (DistanceSquared(point, onCa) < bestDistance) {
        best = onCa;
    }
    return best;
}

// Ericson 영역 판정법(Real-Time Collision Detection 5.1.5)이다. 꼭짓점 → 변 → 면 순서로 Voronoi
// 영역을 판정한다. 이 순서는 앞 영역에서 제외된 조건을 뒤 영역이 전제하므로 바꾸면 안 된다.
// 반올림 안전장치:
// - 외적이 정확히 0인 퇴화 삼각형은 영역 판정의 분모가 0이 되므로 세 변의 최근접점으로 처리한다.
// - 변 영역의 분모(변 길이²)가 상쇄로 0 이하가 되면 해당 변의 시작 꼭짓점을 쓴다.
// - 면 영역은 거의 일직선인 sliver에서 va + vb + vc가 상쇄로 무의미해질 수 있다. 면 투영점이
//   무게중심 좌표 범위 밖이거나 분모가 0 이하면 세 변 결과를 쓰고, 범위 안이어도 세 변 결과가 더
//   가까우면 그 점을 쓴다. 모든 후보가 삼각형 위의 점이므로 이 비교는 결과를 나쁘게 만들지 않는다.
template <typename T>
constexpr Vector3<T> ClosestOnTriangle(const Vector3<T>& point, const Vector3<T>& a,
                                       const Vector3<T>& b, const Vector3<T>& c) noexcept {
    const T zero = static_cast<T>(0);
    const Vector3<T> ab = b - a;
    const Vector3<T> ac = c - a;
    if (Cross(ab, ac) == Vector3<T>::Zero()) {
        return ClosestOnTriangleEdges(point, a, b, c);
    }

    const Vector3<T> ap = point - a;
    const T d1 = Dot(ab, ap);
    const T d2 = Dot(ac, ap);
    if (d1 <= zero && d2 <= zero) {
        return a;
    }

    const Vector3<T> bp = point - b;
    const T d3 = Dot(ab, bp);
    const T d4 = Dot(ac, bp);
    if (d3 >= zero && d4 <= d3) {
        return b;
    }

    const T vc = d1 * d4 - d3 * d2;
    if (vc <= zero && d1 >= zero && d3 <= zero) {
        const T denominator = d1 - d3;
        return denominator > zero ? Lerp(a, b, d1 / denominator) : a;
    }

    const Vector3<T> cp = point - c;
    const T d5 = Dot(ab, cp);
    const T d6 = Dot(ac, cp);
    if (d6 >= zero && d5 <= d6) {
        return c;
    }

    const T vb = d5 * d2 - d1 * d6;
    if (vb <= zero && d2 >= zero && d6 <= zero) {
        const T denominator = d2 - d6;
        return denominator > zero ? Lerp(a, c, d2 / denominator) : a;
    }

    const T va = d3 * d6 - d5 * d4;
    const T toC = d4 - d3;
    const T fromC = d5 - d6;
    if (va <= zero && toC >= zero && fromC >= zero) {
        const T denominator = toC + fromC;
        return denominator > zero ? Lerp(b, c, toC / denominator) : b;
    }

    const Vector3<T> edgeBest = ClosestOnTriangleEdges(point, a, b, c);
    const T denominator = va + vb + vc;
    if (!(denominator > zero)) {
        return edgeBest;
    }

    const T v = vb / denominator;
    const T w = vc / denominator;
    if (!(v >= zero && w >= zero && v + w <= static_cast<T>(1))) {
        return edgeBest;
    }

    const Vector3<T> facePoint = a + ab * v + ac * w;
    if (DistanceSquared(point, edgeBest) < DistanceSquared(point, facePoint)) {
        return edgeBest;
    }
    return facePoint;
}

// 0 길이 segment용 점-삼각형 판정이다. 평면 위(Dot == 0 정확)이고 세 변의 안쪽(경계 포함)이면
// true다.
template <typename T>
constexpr bool PointOnTriangle(const Triangle<T>& triangle, const Vector3<T>& point) noexcept {
    const T zero = static_cast<T>(0);
    const Vector3<T> faceNormal = Cross(triangle.b - triangle.a, triangle.c - triangle.a);
    if (faceNormal == Vector3<T>::Zero() || Dot(faceNormal, point - triangle.a) != zero) {
        return false;
    }

    const T edgeAb = Dot(Cross(triangle.b - triangle.a, point - triangle.a), faceNormal);
    const T edgeBc = Dot(Cross(triangle.c - triangle.b, point - triangle.b), faceNormal);
    const T edgeCa = Dot(Cross(triangle.a - triangle.c, point - triangle.c), faceNormal);
    return edgeAb >= zero && edgeBc >= zero && edgeCa >= zero;
}

// ---- segment 쌍 ----
// Ericson ClosestPtSegmentSegment(5.1.9)를 따른다. 무한 직선 해의 분모 a·e - b²는 거의 평행할 때
// 상쇄가 크므로 같은 값인 |d1 × d2|²로 계산한다. 0 길이와 평행은 숨은 epsilon 없이 정확한 0으로만
// 판정한다. 평행이면 s = 0에서 시작하고, t를 [0,1]로 자른 뒤 s를 다시 구하므로 평행 구간에서도 최소
// 거리를 낸다.
template <typename T>
constexpr SegmentClosestPoints<T> ClosestSegmentSegment(const Segment<T>& first,
                                                        const Segment<T>& second) noexcept {
    const T zero = static_cast<T>(0);
    const Vector3<T> d1 = first.end - first.start;
    const Vector3<T> d2 = second.end - second.start;
    const Vector3<T> r = first.start - second.start;
    const T a = Dot(d1, d1);
    const T e = Dot(d2, d2);
    const T f = Dot(d2, r);

    T s = zero;
    T t = zero;
    if (a == zero && e == zero) {
        // 두 segment가 모두 점이다.
    } else if (a == zero) {
        t = ClampUnit(f / e);
    } else {
        const T c = Dot(d1, r);
        if (e == zero) {
            s = ClampUnit(-c / a);
        } else {
            const Vector3<T> lineNormal = Cross(d1, d2);
            const T denominator = Dot(lineNormal, lineNormal);
            if (denominator > zero) {
                s = ClampUnit(Dot(lineNormal, Cross(d2, r)) / denominator);
            }

            const T b = Dot(d1, d2);
            const T tNumerator = b * s + f;
            if (tNumerator < zero) {
                t = zero;
                s = ClampUnit(-c / a);
            } else if (tNumerator > e) {
                t = static_cast<T>(1);
                s = ClampUnit((b - c) / a);
            } else {
                t = tNumerator / e;
            }
        }
    }

    SegmentClosestPoints<T> result;
    result.s = s;
    result.t = t;
    result.pointA = first.PointAt(s);
    result.pointB = second.PointAt(t);
    result.distanceSquared = DistanceSquared(result.pointA, result.pointB);
    return result;
}

// 대칭 계산용 사전식 순서다. 같은 값(±0 차이 포함)이면 false다.
template <typename T, usize Count>
constexpr bool LexicographicLess(const T (&left)[Count], const T (&right)[Count]) noexcept {
    for (usize index = 0; index < Count; ++index) {
        if (left[index] < right[index]) {
            return true;
        }
        if (right[index] < left[index]) {
            return false;
        }
    }
    return false;
}

template <typename T>
constexpr bool SegmentOrderedBefore(const Segment<T>& left, const Segment<T>& right) noexcept {
    const T leftValues[6] = {left.start.x, left.start.y, left.start.z,
                             left.end.x,   left.end.y,   left.end.z};
    const T rightValues[6] = {right.start.x, right.start.y, right.start.z,
                              right.end.x,   right.end.y,   right.end.z};
    return LexicographicLess(leftValues, rightValues);
}

// 인자 순서와 무관하게 같은 순서로 계산해 (a, b)와 (b, a)의 결과를 비트 단위로 같게 만든다.
template <typename T>
constexpr SegmentClosestPoints<T>
SymmetricClosestSegmentSegment(const Segment<T>& first, const Segment<T>& second) noexcept {
    if (!SegmentOrderedBefore(second, first)) {
        return ClosestSegmentSegment(first, second);
    }

    const SegmentClosestPoints<T> swapped = ClosestSegmentSegment(second, first);
    SegmentClosestPoints<T> result;
    result.s = swapped.t;
    result.t = swapped.s;
    result.pointA = swapped.pointB;
    result.pointB = swapped.pointA;
    result.distanceSquared = swapped.distanceSquared;
    return result;
}

// ---- 평면 분류 ----
// normal 방향으로 SignedDistance가 가장 작은/큰 꼭짓점만 평가한다. center·extent를 거치지 않으므로
// 꼭짓점이 평면 위에 정확히 있으면 정확히 Intersecting이다.
template <typename T>
constexpr PlaneSide ClassifyBoxUnchecked(const Plane<T>& plane, const Box<T>& box) noexcept {
    const T zero = static_cast<T>(0);
    const Vector3<T> lowest(plane.normal.x >= zero ? box.min.x : box.max.x,
                            plane.normal.y >= zero ? box.min.y : box.max.y,
                            plane.normal.z >= zero ? box.min.z : box.max.z);
    const Vector3<T> highest(plane.normal.x >= zero ? box.max.x : box.min.x,
                             plane.normal.y >= zero ? box.max.y : box.min.y,
                             plane.normal.z >= zero ? box.max.z : box.min.z);
    if (plane.SignedDistance(lowest) > zero) {
        return PlaneSide::Front;
    }
    if (plane.SignedDistance(highest) < zero) {
        return PlaneSide::Back;
    }
    return PlaneSide::Intersecting;
}

template <typename T>
constexpr PlaneSide ClassifySphereUnchecked(const Plane<T>& plane,
                                            const Sphere<T>& sphere) noexcept {
    const T signedDistance = plane.SignedDistance(sphere.center);
    if (signedDistance > sphere.radius) {
        return PlaneSide::Front;
    }
    if (signedDistance < -sphere.radius) {
        return PlaneSide::Back;
    }
    return PlaneSide::Intersecting;
}

// 축 선분 두 끝점의 SignedDistance 중 작은 값과 큰 값이다. Capsule·Cylinder의 평면 support를
// 중심 ± (H/2)|n·a| 대신 끝점 거리로 구한다(같은 값이며 중심 계산의 반올림이 없고, 끝점이 평면
// 위면 그 거리가 정확히 0이다).
template <typename T>
struct PlaneSegmentDistances {
    T lowest;
    T highest;
};

template <typename T>
constexpr PlaneSegmentDistances<T> PlaneSegmentRange(const Plane<T>& plane, const Vector3<T>& start,
                                                     const Vector3<T>& end) noexcept {
    const T startDistance = plane.SignedDistance(start);
    const T endDistance = plane.SignedDistance(end);
    return PlaneSegmentDistances<T>{math::Min(startDistance, endDistance),
                                    math::Max(startDistance, endDistance)};
}

// 축 선분을 따라 쓴 원판·구(support 반폭 discSupport)의 평면 분류다. 거리에서 반폭을 빼지 않고
// 반폭과 비교하므로 0 길이 capsule은 sphere 분류와 비트 단위로 같다.
template <typename T>
constexpr PlaneSide ClassifySweptUnchecked(const Plane<T>& plane, const Vector3<T>& start,
                                           const Vector3<T>& end, T discSupport) noexcept {
    const PlaneSegmentDistances<T> range = PlaneSegmentRange(plane, start, end);
    if (range.lowest > discSupport) {
        return PlaneSide::Front;
    }
    if (range.highest < -discSupport) {
        return PlaneSide::Back;
    }
    return PlaneSide::Intersecting;
}

// ---- OrientedBox 분리축 ----
template <typename T>
constexpr bool OrientedBoxOrderedBefore(const OrientedBox<T>& left,
                                        const OrientedBox<T>& right) noexcept {
    const T leftValues[10] = {left.center.x,     left.center.y,      left.center.z,
                              left.rotation.x,   left.rotation.y,    left.rotation.z,
                              left.rotation.w,   left.halfExtents.x, left.halfExtents.y,
                              left.halfExtents.z};
    const T rightValues[10] = {right.center.x,     right.center.y,      right.center.z,
                               right.rotation.x,   right.rotation.y,    right.rotation.z,
                               right.rotation.w,   right.halfExtents.x, right.halfExtents.y,
                               right.halfExtents.z};
    return LexicographicLess(leftValues, rightValues);
}

// Ericson 4.4.1의 15축 분리축 판정이다(A의 면 3, B의 면 3, 모서리 외적 9). 모든 값은 A의 local 축
// 기준이다. 투영 간격이 반지름 합보다 엄격히 클 때만 분리이므로 접촉은 겹침이다. 회전 행렬 원소의
// 절댓값에 parallelEpsilon을 더해 거의 평행한 모서리의 외적 축(길이가 0에 가까움)이 반올림
// 잡음만으로 분리축이 되는 것을 막는다. 외적 축은 정규화하지 않아도 양변이 같은 배율이라 비교가
// 성립한다.
template <typename T>
bool OrientedBoxesSeparated(const OrientedBox<T>& first, const OrientedBox<T>& second,
                            T parallelEpsilon) noexcept {
    // IsValid는 길이 오차를 허용하므로 축은 정규화한 회전에서 구한다(OrientedBox::UnitRotation).
    const Quaternion<T> rotationA = first.UnitRotation();
    const Quaternion<T> rotationB = second.UnitRotation();
    const Vector3<T> axesA[3] = {rotationA.RotateVector(Vector3<T>::UnitX()),
                                 rotationA.RotateVector(Vector3<T>::UnitY()),
                                 rotationA.RotateVector(Vector3<T>::UnitZ())};
    const Vector3<T> axesB[3] = {rotationB.RotateVector(Vector3<T>::UnitX()),
                                 rotationB.RotateVector(Vector3<T>::UnitY()),
                                 rotationB.RotateVector(Vector3<T>::UnitZ())};
    const T extentA[3] = {first.halfExtents.x, first.halfExtents.y, first.halfExtents.z};
    const T extentB[3] = {second.halfExtents.x, second.halfExtents.y, second.halfExtents.z};

    T rotation[3][3];
    T absRotation[3][3];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            rotation[i][j] = Dot(axesA[i], axesB[j]);
            absRotation[i][j] = math::Abs(rotation[i][j]) + parallelEpsilon;
        }
    }

    const Vector3<T> centerOffset = second.center - first.center;
    const T offset[3] = {Dot(centerOffset, axesA[0]), Dot(centerOffset, axesA[1]),
                         Dot(centerOffset, axesA[2])};

    for (int i = 0; i < 3; ++i) {
        const T radiusA = extentA[i];
        const T radiusB = extentB[0] * absRotation[i][0] + extentB[1] * absRotation[i][1] +
                          extentB[2] * absRotation[i][2];
        if (math::Abs(offset[i]) > radiusA + radiusB) {
            return true;
        }
    }

    for (int j = 0; j < 3; ++j) {
        const T radiusA = extentA[0] * absRotation[0][j] + extentA[1] * absRotation[1][j] +
                          extentA[2] * absRotation[2][j];
        const T radiusB = extentB[j];
        const T projected =
            offset[0] * rotation[0][j] + offset[1] * rotation[1][j] + offset[2] * rotation[2][j];
        if (math::Abs(projected) > radiusA + radiusB) {
            return true;
        }
    }

    // 축 A_i × B_j. i1, i2와 j1, j2는 각각 i, j 다음의 순환 index다.
    for (int i = 0; i < 3; ++i) {
        const int i1 = (i + 1) % 3;
        const int i2 = (i + 2) % 3;
        for (int j = 0; j < 3; ++j) {
            const int j1 = (j + 1) % 3;
            const int j2 = (j + 2) % 3;
            const T radiusA = extentA[i1] * absRotation[i2][j] + extentA[i2] * absRotation[i1][j];
            const T radiusB = extentB[j1] * absRotation[i][j2] + extentB[j2] * absRotation[i][j1];
            const T projected = offset[i2] * rotation[i1][j] - offset[i1] * rotation[i2][j];
            if (math::Abs(projected) > radiusA + radiusB) {
                return true;
            }
        }
    }
    return false;
}

// 평면 기준으로 box의 가장 뒤 꼭짓점과 가장 앞 꼭짓점의 SignedDistance다(ClassifyBoxUnchecked와
// 같은 꼭짓점 선택).
template <typename T>
struct PlaneBoxDistances {
    T lowest;
    T highest;
};

template <typename T>
constexpr PlaneBoxDistances<T> PlaneBoxRange(const Plane<T>& plane, const Box<T>& box) noexcept {
    const T zero = static_cast<T>(0);
    const Vector3<T> lowest(plane.normal.x >= zero ? box.min.x : box.max.x,
                            plane.normal.y >= zero ? box.min.y : box.max.y,
                            plane.normal.z >= zero ? box.min.z : box.max.z);
    const Vector3<T> highest(plane.normal.x >= zero ? box.max.x : box.min.x,
                             plane.normal.y >= zero ? box.max.y : box.min.y,
                             plane.normal.z >= zero ? box.max.z : box.min.z);
    return PlaneBoxDistances<T>{plane.SignedDistance(lowest), plane.SignedDistance(highest)};
}

// box에서 point까지 가장 먼 꼭짓점의 제곱 거리다. 축마다 먼 쪽 면까지의 거리를 고른다.
template <typename T>
constexpr T BoxFarthestDistanceSquared(const Vector3<T>& minimum, const Vector3<T>& maximum,
                                       const Vector3<T>& point) noexcept {
    const Vector3<T> toMinimum = point - minimum;
    const Vector3<T> toMaximum = maximum - point;
    const Vector3<T> farthest(math::Max(math::Abs(toMinimum.x), math::Abs(toMaximum.x)),
                              math::Max(math::Abs(toMinimum.y), math::Abs(toMaximum.y)),
                              math::Max(math::Abs(toMinimum.z), math::Abs(toMaximum.z)));
    return farthest.LengthSquared();
}

template <typename T>
constexpr Containment ClassifyVolumeBoxUnchecked(const ConvexVolume<T>& volume,
                                                 const Box<T>& box) noexcept {
    const T zero = static_cast<T>(0);
    bool inside = true;
    for (uint32 index = 0; index < volume.planeCount; ++index) {
        const PlaneBoxDistances<T> range = PlaneBoxRange(volume.planes[index], box);
        if (range.highest < zero) {
            return Containment::Outside;
        }
        inside = inside && range.lowest >= zero;
    }
    return inside ? Containment::Inside : Containment::Intersecting;
}

template <typename T>
constexpr Containment ClassifyVolumeSphereUnchecked(const ConvexVolume<T>& volume,
                                                    const Sphere<T>& sphere) noexcept {
    bool inside = true;
    for (uint32 index = 0; index < volume.planeCount; ++index) {
        const T signedDistance = volume.planes[index].SignedDistance(sphere.center);
        if (signedDistance < -sphere.radius) {
            return Containment::Outside;
        }
        inside = inside && signedDistance >= sphere.radius;
    }
    return inside ? Containment::Inside : Containment::Intersecting;
}

template <typename T>
constexpr Containment ClassifySphereBoxUnchecked(const Sphere<T>& sphere,
                                                 const Box<T>& box) noexcept {
    const T radiusSquared = sphere.radius * sphere.radius;
    if (BoxDistanceSquared(box.min, box.max, sphere.center) > radiusSquared) {
        return Containment::Outside;
    }
    return BoxFarthestDistanceSquared(box.min, box.max, sphere.center) <= radiusSquared
               ? Containment::Inside
               : Containment::Intersecting;
}

// 평면마다 축 선분 끝점 거리 범위와 support 반폭을 비교한다(ClassifySweptUnchecked와 같은 식).
// cylinder는 평면 normal마다 반폭 r·|n × a|가 달라 SupportFunction으로 받는다.
template <typename T, typename SupportFunction>
constexpr Containment ClassifyVolumeSweptUnchecked(const ConvexVolume<T>& volume,
                                                   const Vector3<T>& start, const Vector3<T>& end,
                                                   SupportFunction support) noexcept {
    bool inside = true;
    for (uint32 index = 0; index < volume.planeCount; ++index) {
        const Plane<T>& plane = volume.planes[index];
        const T discSupport = support(plane);
        const PlaneSegmentDistances<T> range = PlaneSegmentRange(plane, start, end);
        if (range.highest < -discSupport) {
            return Containment::Outside;
        }
        inside = inside && range.lowest >= discSupport;
    }
    return inside ? Containment::Inside : Containment::Intersecting;
}

} // namespace detail

} // namespace rocks
