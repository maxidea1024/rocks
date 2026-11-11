#pragma once

#include <Core/Math/Shapes.h>

namespace rocks {

// 기본 shape의 교차·포함·거리 질의다. 모든 함수는 상태 없는 순수 함수이며 어느 스레드에서나 호출할
// 수 있다.
//
// 결과 계약
// - 입력 오류와 접촉 결과를 구분한다. shape·ray·point가 IsValid가 아니거나(비유한값, 음수 반지름,
//   역전된 box, 단위가 아닌 방향·회전) maxDistance가 NaN·음수이면 QueryResult::InvalidInput을
//   반환하며 Miss나 Hit로 대신하지 않는다. 최근접점·거리 함수는 같은 경우 Result
//   실패(InvalidArgument)다.
// - shape는 닫힌 집합이다. 경계에 정확히 닿으면 Hit다. 비교에 숨은 epsilon이 없으며 허용 오차가
//   필요한 질의(OrientedBox 쌍)는 명시적 인자로 받는다. 따라서 경계 근처 결과는 T 반올림에 따라
//   달라질 수 있다.
// - 대칭: Overlap(a, b)와 Overlap(b, a)는 항상 같은 결과다. 서로 다른 타입 쌍은 두 순서를 모두
//   제공하고 같은 구현을 호출한다. 같은 타입 쌍(Capsule, OrientedBox, segment 최근접점)은 인자를
//   정해진 사전식 순서로 바꿔 계산하므로 순서를 바꿔도 비트 단위로 같은 값이다.
// - Ray 질의: direction은 단위 벡터여야 하고 distance는 origin에서 잰 실제 거리로
//   [0, maxDistance]에 있다. maxDistance는 +Inf일 수 있으며 maxDistance와 같은 거리의 접촉도 Hit다.
//   -0은 0으로 취급한다.
// - 시작점 내부: origin(segment는 start)이 solid(Sphere, Box, Capsule, OrientedBox)의 내부나 경계
//   위면 거리 0, point = origin, normal = 영벡터, startedInside = true인 Hit다. 표면에서 바깥으로
//   쏘는 ray도 경계 위이므로 거리 0 Hit가 된다. UE LineExtentBoxIntersection은 같은 상황에서 normal
//   (0,0,1)을 돌려주고 LineBoxIntersection은 bool만 돌려준다. Rocks는 의미 없는 방향 대신 영벡터로
//   표시한다.
// - normal: solid 표면 hit은 바깥 방향 단위 normal(반올림 수준)이다. Plane·Triangle hit은 진행
//   방향의 반대쪽, 즉 ray가 출발한 쪽을 향한다. Plane·Triangle은 두께가 없으므로 startedInside는
//   항상 false다.
// - Segment 질의: t는 [0,1] 매개변수다(거리 아님). Sphere·Box·Triangle은
//   start + (end - start) * t로 계산하므로 end - start가 반올림되면 t = 1의 점이 end와 1 ulp
//   수준으로 달라질 수 있다(예: start=-5, end=-1-ulp이면 차이가 정확히 4로 반올림되어 표면 -1에
//   닿는다). Plane은 끝점별 거리로 계산해 끝점이 평면 위면 t를 정확히 0/1로 낸다. 0 길이 segment는
//   점 판정이다. Plane, Sphere, Box, Triangle만 지원한다.
// - 지원하지 않는 범위: frustum, swept shape(sweep), 접촉 깊이·다중 접촉점, mesh 가속 구조. 물리
//   응답은 별도 계층에서 다룬다.

// 겹침·포함 판정 결과다. 입력 오류(InvalidInput)는 접촉 여부(Miss/Hit)와 구분한다.
enum class QueryResult : uint8 {
    Miss,
    Hit,
    InvalidInput
};

// 평면에 대한 위치 분류다. Front/Back은 shape 전체가 엄격히 앞(SignedDistance > 0)/뒤(< 0)에 있을
// 때만이며 평면에 닿기만 해도 Intersecting이다.
enum class PlaneSide : uint8 {
    Front,
    Back,
    Intersecting,
    InvalidInput
};

// 삼각형 ray·segment 질의의 면 선택이다. TwoSided(기본)는 양면 모두 Hit, FrontOnly는 앞면
// (Cross(b - a, c - a) 쪽)에서 들어오는 경우만 Hit다.
enum class TriangleSides : uint8 {
    TwoSided,
    FrontOnly
};

// Ray 질의 결과다. IsHit()이 false면 나머지 필드는 기본값(0)이며 의미가 없다.
template <typename T>
struct RayHit {
    QueryResult result = QueryResult::Miss;
    T distance = static_cast<T>(0);
    Vector3<T> point;
    Vector3<T> normal;
    bool startedInside = false;

    constexpr bool IsHit() const noexcept {
        return result == QueryResult::Hit;
    }
};

// Segment 질의 결과다. t는 [0,1] 매개변수이며 거리 = t * segment 길이다. IsHit()이 false면 나머지
// 필드는 기본값이다.
template <typename T>
struct SegmentHit {
    QueryResult result = QueryResult::Miss;
    T t = static_cast<T>(0);
    Vector3<T> point;
    Vector3<T> normal;
    bool startedInside = false;

    constexpr bool IsHit() const noexcept {
        return result == QueryResult::Hit;
    }
};

// 두 segment의 최근접점이다. s는 첫 segment, t는 두 번째 segment의 [0,1] 매개변수다.
// 최소 거리를 주는 점 쌍이 여러 개(평행 구간)면 그중 하나를 돌려준다.
template <typename T>
struct SegmentClosestPoints {
    T s = static_cast<T>(0);
    T t = static_cast<T>(0);
    Vector3<T> pointA;
    Vector3<T> pointB;
    T distanceSquared = static_cast<T>(0);
};

// OrientedBox 쌍 분리축 검사의 기본 평행 보정값이다. 두 box의 회전 행렬 원소 절댓값에 더해서, 거의
// 평행한 모서리의 외적 축이 반올림 잡음만으로 분리축이 되는 것을 막는다. 판정은 이 값 * extent
// 정도만큼 겹침 쪽으로 보수적이다(아주 작은 틈은 Hit일 수 있다).
template <typename T>
    requires math::IsFloatingPointV<T>
inline constexpr T ObbParallelEpsilon =
    IsSameV<T, float> ? static_cast<T>(1.0e-6) : static_cast<T>(1.0e-12);

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
// 수 있다 (watertight 판정은 보장하지 않는다). 퇴화 삼각형과 det == 0(평행·공면)은 Miss다. det가
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
    if (det == zero) {
        return {};
    }

    const bool frontFace = det > zero;
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

    const T t = scaledT / det;
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
    if (!(a > zero) || b >= zero || startGap <= zero) {
        // 축과 평행하거나, 축에서 멀어지거나, 이미 무한 원기둥 안(축 범위 밖)이면 끝 구가 첫
        // 진입이다.
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

// box local 좌표로 바꿔 slab 판정을 한다. 회전은 길이를 보존하므로 t는 world 거리와 같다.
template <typename T>
CastResult<T> CastOrientedBox(const Vector3<T>& origin, const Vector3<T>& delta, T tMax,
                              const OrientedBox<T>& box) noexcept {
    const Vector3<T> localOrigin = box.ToLocal(origin);
    const Vector3<T> localDelta = box.rotation.UnrotateVector(delta);
    CastResult<T> cast = CastBox(localOrigin, localDelta, tMax, -box.halfExtents, box.halfExtents);
    if (cast.result != QueryResult::Hit) {
        return cast;
    }
    if (cast.startedInside) {
        return CastInside(origin);
    }

    cast.point = box.ToWorld(cast.point);
    cast.normal = box.rotation.RotateVector(cast.normal);
    return cast;
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
    const Vector3<T> axesA[3] = {first.AxisX(), first.AxisY(), first.AxisZ()};
    const Vector3<T> axesB[3] = {second.AxisX(), second.AxisY(), second.AxisZ()};
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

} // namespace detail

// ===== Ray 질의 =====
// 모든 Raycast는 ray.IsValid(), shape.IsValid(), maxDistance >= 0(NaN 제외, +Inf 허용)을 요구하며
// 위반이면 InvalidInput이다. 접촉 거리는 [0, maxDistance]다.

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

    const T distance = -startDistance / rate;
    if (distance < zero || distance > maxDistance) {
        return {};
    }

    const Vector3<T> normal = startDistance > zero ? plane.normal : -plane.normal;
    return detail::ToRayHit(detail::CastHit(distance, ray.PointAt(distance), normal));
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

// 0 길이 segment는 점이 삼각형 위(평면 위 정확 판정, 경계 포함)이면 t = 0 Hit이며 normal은 앞면
// normal이다. UE SegmentTriangleIntersection과 달리 평면 매개변수에 숨은 허용 오차가 없다.
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
    return detail::ToSegmentHit(
        detail::CastTriangle(segment.start, delta, static_cast<T>(1), triangle, sides));
}

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

// ===== 최근접점·거리 =====
// 잘못된 shape나 유한하지 않은 point는 InvalidArgument 실패다.

// 평면 위 수직 투영점이다.
template <typename T>
Result<Vector3<T>> TryClosestPoint(const Plane<T>& plane, const Vector3<T>& point) noexcept {
    if (!plane.IsValid() || !point.IsFinite()) {
        return Result<Vector3<T>>::Failure(ErrorCode::InvalidArgument);
    }
    return Result<Vector3<T>>::Success(plane.ProjectPoint(point));
}

// Dot(normal, point) - distance다.
template <typename T>
Result<T> TrySignedDistance(const Plane<T>& plane, const Vector3<T>& point) noexcept {
    if (!plane.IsValid() || !point.IsFinite()) {
        return Result<T>::Failure(ErrorCode::InvalidArgument);
    }
    return Result<T>::Success(plane.SignedDistance(point));
}

// 투영이 끝점 밖이면 끝점을 정확히 돌려준다. 0 길이 segment는 start다.
template <typename T>
Result<Vector3<T>> TryClosestPoint(const Segment<T>& segment, const Vector3<T>& point) noexcept {
    if (!segment.IsValid() || !point.IsFinite()) {
        return Result<Vector3<T>>::Failure(ErrorCode::InvalidArgument);
    }
    return Result<Vector3<T>>::Success(detail::ClosestOnSegment(point, segment.start, segment.end));
}

template <typename T>
Result<T> TryDistanceSquared(const Segment<T>& segment, const Vector3<T>& point) noexcept {
    if (!segment.IsValid() || !point.IsFinite()) {
        return Result<T>::Failure(ErrorCode::InvalidArgument);
    }
    return Result<T>::Success(detail::SegmentDistanceSquared(point, segment.start, segment.end));
}

// 축별 clamp다. box 안의 점은 그 점 자체를 정확히 돌려준다.
template <typename T>
Result<Vector3<T>> TryClosestPoint(const Box<T>& box, const Vector3<T>& point) noexcept {
    if (!box.IsValid() || !point.IsFinite()) {
        return Result<Vector3<T>>::Failure(ErrorCode::InvalidArgument);
    }
    return Result<Vector3<T>>::Success(detail::ClampToBox(box.min, box.max, point));
}

template <typename T>
Result<T> TryDistanceSquared(const Box<T>& box, const Vector3<T>& point) noexcept {
    if (!box.IsValid() || !point.IsFinite()) {
        return Result<T>::Failure(ErrorCode::InvalidArgument);
    }
    return Result<T>::Success(detail::BoxDistanceSquared(box.min, box.max, point));
}

// Ericson 영역 판정법과 퇴화·sliver 안전장치(detail::ClosestOnTriangle)다. 결과는 항상 삼각형 위의
// 점이다.
template <typename T>
Result<Vector3<T>> TryClosestPoint(const Triangle<T>& triangle, const Vector3<T>& point) noexcept {
    if (!triangle.IsValid() || !point.IsFinite()) {
        return Result<Vector3<T>>::Failure(ErrorCode::InvalidArgument);
    }
    return Result<Vector3<T>>::Success(
        detail::ClosestOnTriangle(point, triangle.a, triangle.b, triangle.c));
}

template <typename T>
Result<T> TryDistanceSquared(const Triangle<T>& triangle, const Vector3<T>& point) noexcept {
    if (!triangle.IsValid() || !point.IsFinite()) {
        return Result<T>::Failure(ErrorCode::InvalidArgument);
    }
    const Vector3<T> closest = detail::ClosestOnTriangle(point, triangle.a, triangle.b, triangle.c);
    return Result<T>::Success(DistanceSquared(point, closest));
}

// local 좌표에서 clamp한 뒤 world로 되돌린다. box 안(local 판정)의 점은 그 점 자체를 정확히
// 돌려준다.
template <typename T>
Result<Vector3<T>> TryClosestPoint(const OrientedBox<T>& box, const Vector3<T>& point) noexcept {
    if (!box.IsValid() || !point.IsFinite()) {
        return Result<Vector3<T>>::Failure(ErrorCode::InvalidArgument);
    }

    const Vector3<T> local = box.ToLocal(point);
    if (detail::BoxContains(-box.halfExtents, box.halfExtents, local)) {
        return Result<Vector3<T>>::Success(point);
    }
    return Result<Vector3<T>>::Success(
        box.ToWorld(detail::ClampToBox(-box.halfExtents, box.halfExtents, local)));
}

// local 좌표에서 계산하므로 world로 되돌리는 반올림이 거리에 섞이지 않는다.
template <typename T>
Result<T> TryDistanceSquared(const OrientedBox<T>& box, const Vector3<T>& point) noexcept {
    if (!box.IsValid() || !point.IsFinite()) {
        return Result<T>::Failure(ErrorCode::InvalidArgument);
    }
    const Vector3<T> local = box.ToLocal(point);
    return Result<T>::Success(detail::BoxDistanceSquared(-box.halfExtents, box.halfExtents, local));
}

// 두 segment의 최근접점이다. 평행·0 길이 segment도 처리하며 인자 순서를 바꾸면 s/t와 점만 바뀌고
// 거리는 비트 단위로 같다. UE SegmentDistToSegmentSafe의 절대 허용 오차(1e-4) 대신 정확한 0 판정을
// 쓴다.
template <typename T>
Result<SegmentClosestPoints<T>> TryClosestPoints(const Segment<T>& first,
                                                 const Segment<T>& second) noexcept {
    if (!first.IsValid() || !second.IsValid()) {
        return Result<SegmentClosestPoints<T>>::Failure(ErrorCode::InvalidArgument);
    }
    return Result<SegmentClosestPoints<T>>::Success(
        detail::SymmetricClosestSegmentSegment(first, second));
}

} // namespace rocks
