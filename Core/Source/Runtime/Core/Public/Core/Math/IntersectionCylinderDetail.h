#pragma once

#include <Base/NumericLimits.h>
#include <Core/Math/IntersectionCastDetail.h>

namespace rocks {

// cylinder 판정의 내부 구현이다: 축 좌표 분해, 거리·최근접점, cast, 경계 여유 상수, 평면 support.

namespace detail {

// ---- cylinder ----
// 성분 제곱이 underflow·overflow하는 벡터도 가장 큰 성분으로 먼저 나눠 길이를 구한다
// (Vector3::TryNormalize와 같은 방식). 영벡터는 0이다.
template <typename T>
T StableLength(const Vector3<T>& vector) noexcept {
    const T largest = vector.MaxAbsComponent();
    if (!(largest > static_cast<T>(0))) {
        return static_cast<T>(0);
    }
    return largest * (vector / largest).Length();
}

// radius > 0인 유효 cylinder의 단위 축이다. IsValid가 정규화 성공을 보장하므로 대체값은 쓰이지
// 않는다. 한 판정 안에서는 이 값 하나를 모든 계산에 쓴다.
template <typename T>
Vector3<T> CylinderUnitAxis(const Cylinder<T>& cylinder) noexcept {
    return (cylinder.end - cylinder.start).NormalizedOr(Vector3<T>::Zero());
}

// 점의 cylinder 좌표다. fromStart = (p - start)·a, fromEnd = (p - end)·a이고 radial은 축에 수직인
// 성분 (p - start) - fromStart·a다. 끝면 판정을 높이 H와의 비교 대신 각 끝점 기준 내적의 부호로
// 하므로 p == start, p == end에서 축 방향 값이 정확히 0이다. 수직 성분은 |p - start|² -
// fromStart² 꼴(긴 축에서 소거로 망가진다) 대신 벡터 차로 구한다.
template <typename T>
struct CylinderCoordinates {
    T fromStart;
    T fromEnd;
    Vector3<T> radial;
};

template <typename T>
constexpr CylinderCoordinates<T> ToCylinderCoordinates(const Cylinder<T>& cylinder,
                                                       const Vector3<T>& axis,
                                                       const Vector3<T>& point) noexcept {
    const Vector3<T> offset = point - cylinder.start;
    const T fromStart = Dot(offset, axis);
    return CylinderCoordinates<T>{fromStart, Dot(point - cylinder.end, axis),
                                  offset - axis * fromStart};
}

// 수직 성분이 반지름 안(|radial|² <= r²)인지다. 점 포함과 cast의 시작점·옆면 판정이 모두 이 식을
// 써서 같은 점에 대한 판단이 엇갈리지 않는다. r²이나 |radial|²이 overflow하는 반지름·거리(double 약
// 1e154 이상, 좌표 계약 1e75 밖)에서는 Inf 비교가 되어 결과를 보장하지 않는다.
template <typename T>
constexpr bool CylinderRadialInside(const Vector3<T>& radial, T radius) noexcept {
    return Dot(radial, radial) <= radius * radius;
}

// 점 포함 식이다. ContainsPoint, cast의 시작점 판정, 거리·최근접점이 모두 이 식을 써서 한 점에
// 대한 안·밖 판단이 판정마다 엇갈리지 않는다.
template <typename T>
constexpr bool CylinderCoordinatesInside(const CylinderCoordinates<T>& coordinates,
                                         T radius) noexcept {
    const T zero = static_cast<T>(0);
    return coordinates.fromStart >= zero && coordinates.fromEnd <= zero &&
           CylinderRadialInside(coordinates.radial, radius);
}

// cylinder 점 포함의 공통 식이다(ContainsPoint와 거리·선분 끝점 보정이 함께 쓴다). radius 0은
// CastCapsule의 시작 접촉 판정(CapsuleCastStartsInside)과 같은 식이라 Raycast의 startedInside와
// 엇갈리지 않는다.
template <typename T>
bool CylinderContainsPoint(const Cylinder<T>& cylinder, const Vector3<T>& point) noexcept {
    if (!(cylinder.radius > static_cast<T>(0))) {
        return CapsuleCastStartsInside(point,
                                       Capsule<T>(cylinder.start, cylinder.end, static_cast<T>(0)));
    }
    return CylinderCoordinatesInside(
        ToCylinderCoordinates(cylinder, CylinderUnitAxis(cylinder), point), cylinder.radius);
}

// 곱집합의 성분별 투영이다. 축 방향은 끝면 밖으로 나간 거리, 수직 방향은 반지름 밖으로 나간
// 거리이며 둘의 제곱합이 거리²다(끝면 모서리 영역에서도 정확하다). 안(CylinderContainsPoint)의 점은
// 정확히 0이고, 밖의 점은 반올림·underflow로 제곱합이 그보다 작아져도 가장 작은 양의 정규
// 값(NumericLimits<T>::Min())으로 올려 "거리² == 0 ⇔ 포함"을 지킨다. 그래서 밖의 점에서 실제
// 거리²가 그 값보다 작으면 결과가 실제보다 크다.
template <typename T>
T CylinderDistanceSquared(const Cylinder<T>& cylinder, const Vector3<T>& point) noexcept {
    const T zero = static_cast<T>(0);
    if (!(cylinder.radius > zero)) {
        if (CylinderContainsPoint(cylinder, point)) {
            return zero;
        }
        return math::Max(SegmentDistanceSquared(point, cylinder.start, cylinder.end),
                         NumericLimits<T>::Min());
    }

    const CylinderCoordinates<T> coordinates =
        ToCylinderCoordinates(cylinder, CylinderUnitAxis(cylinder), point);
    if (CylinderCoordinatesInside(coordinates, cylinder.radius)) {
        return zero;
    }

    T axialGap = zero;
    if (coordinates.fromStart < zero) {
        axialGap = -coordinates.fromStart;
    } else if (coordinates.fromEnd > zero) {
        axialGap = coordinates.fromEnd;
    }
    const T radialGap = math::Max(StableLength(coordinates.radial) - cylinder.radius, zero);
    return math::Max(axialGap * axialGap + radialGap * radialGap, NumericLimits<T>::Min());
}

// ClosestOnCylinder의 안쪽 보정 이분 탐색 횟수 상한이다. double 가수 폭(53비트)보다 많아 매개변수
// 간격이 한 ulp 아래로 줄어들고, float는 간격이 더 줄지 않으면 먼저 끝난다.
inline constexpr uint32 CylinderClosestPointBisectionSteps = 64;

// 축 성분은 끝면으로, 수직 성분은 반지름으로 자른다. 안의 점은 그 점 자체를 정확히 돌려준다.
// 잘라 만든 점이 반올림으로 포함 식(CylinderCoordinatesInside)의 밖이 되면, 포함 식이 안으로 보는
// 내부 기준점(중심, 아니면 start — start는 축 좌표가 정확히 0이라 항상 안)과 그 점 사이를 이분
// 탐색해 안으로 판단되는 가장 바깥 점을 돌려준다. 기준점에서 경계까지의 선분은 볼록 집합의 내부를
// 지나므로 결과는 반올림 수준으로 경계 위의 최근접점이며 ContainsPoint는 항상 Hit다. radius 0은
// 포함 식(CylinderContainsPoint)이 안이면 그 점 자체, 아니면 선분 위 최근접점이며 이 경우
// 반올림 때문에 ContainsPoint가 Hit가 아닐 수 있다(두께 없는 선분 위 점의 근사).
template <typename T>
Vector3<T> ClosestOnCylinder(const Cylinder<T>& cylinder, const Vector3<T>& point) noexcept {
    const T zero = static_cast<T>(0);
    if (!(cylinder.radius > zero)) {
        if (CylinderContainsPoint(cylinder, point)) {
            return point;
        }
        return ClosestOnSegment(point, cylinder.start, cylinder.end);
    }

    const Vector3<T> axis = CylinderUnitAxis(cylinder);
    const CylinderCoordinates<T> coordinates = ToCylinderCoordinates(cylinder, axis, point);
    if (CylinderCoordinatesInside(coordinates, cylinder.radius)) {
        return point;
    }

    const T radialLength = StableLength(coordinates.radial);
    const Vector3<T> radial = radialLength > cylinder.radius
                                  ? coordinates.radial * (cylinder.radius / radialLength)
                                  : coordinates.radial;
    Vector3<T> candidate = (point - coordinates.radial) + radial;
    if (coordinates.fromStart < zero) {
        candidate = cylinder.start + radial;
    } else if (coordinates.fromEnd > zero) {
        candidate = cylinder.end + radial;
    }

    const auto inside = [&](const Vector3<T>& value) noexcept {
        return CylinderCoordinatesInside(ToCylinderCoordinates(cylinder, axis, value),
                                         cylinder.radius);
    };
    if (inside(candidate)) {
        return candidate;
    }

    const Vector3<T> center = cylinder.Center();
    const Vector3<T> anchor = inside(center) ? center : cylinder.start;
    T low = zero;
    T high = static_cast<T>(1);
    Vector3<T> best = anchor;
    for (uint32 step = 0; step < CylinderClosestPointBisectionSteps; ++step) {
        const T middle = low + (high - low) * static_cast<T>(0.5);
        if (!(middle > low && middle < high)) {
            break;
        }
        const Vector3<T> probe = Lerp(anchor, candidate, middle);
        if (inside(probe)) {
            low = middle;
            best = probe;
        } else {
            high = middle;
        }
    }
    return best;
}

// 안(경계 포함)의 점에서 가장 가까운 경계 부분의 바깥 normal이다. 끝면과 옆면까지의 여유가 같으면
// 끝면을 고른다. 선분 끝점 접촉 보정에 쓴다.
template <typename T>
Vector3<T> CylinderNearestBoundaryNormal(const Cylinder<T>& cylinder, const Vector3<T>& axis,
                                         const CylinderCoordinates<T>& coordinates,
                                         const Vector3<T>& delta) noexcept {
    const T toStartCap = coordinates.fromStart;
    const T toEndCap = -coordinates.fromEnd;
    const T toSide = cylinder.radius - StableLength(coordinates.radial);
    if (toStartCap <= toEndCap && toStartCap <= toSide) {
        return -axis;
    }
    if (toEndCap <= toSide) {
        return axis;
    }
    return coordinates.radial.NormalizedOr(OpposingDirection(delta));
}

// 진입 경계의 바깥 normal이다. 끝면은 ±축, 옆면은 hit 시각의 수직 성분 방향이다. 옆면 normal은
// 접선에 가까운 진입에서 반올림으로 진행 방향과 아주 조금 같은 쪽을 향할 수 있다(근사).
template <typename T>
Vector3<T> CylinderEntryNormal(bool throughCap, T axialRate, const Vector3<T>& axis,
                               const Vector3<T>& radialAtHit, const Vector3<T>& delta) noexcept {
    if (throughCap) {
        return axialRate > static_cast<T>(0) ? -axis : axis;
    }
    return radialAtHit.NormalizedOr(OpposingDirection(delta));
}

// 구간 교집합 판정이다(Docs/Design/CylinderShape.ko.md 3.5절, CastBox의 slab과 같은 방식).
// 1. 축 slab: 두 끝면 평면 사이에 있는 t 구간. 축 방향 속도가 정확히 0이면 slab 안일 때만
//    전체 구간.
// 2. 옆면: 수직 성분 |radial + t·deltaRadial|² <= r²의 2차식 구간. 근은 CastSphere와 같이 수직
//    벡터로 판별식을 구하고 소거 없는 근 공식으로 계산한다. 수직 속도가 정확히 0이면 무한 원기둥
//    안일 때만 전체 구간.
// 3. 진입 = 두 하한 중 큰 것(시작점 쪽 0 포함), 이탈 = min(두 상한, tMax). 진입 <= 이탈이면 Hit.
// 진입을 정한 경계의 바깥 normal을 쓰며 끝면과 옆면 진입 시각이 같으면(모서리) 끝면 normal이다.
// 시작점 안·밖은 CylinderCoordinatesInside와 같은 식(옆면은 CylinderRadialInside)으로 판단하고,
// 밖인데 진입 시각이 반올림·underflow로 0이 되면 거리 0, 진입 경계 normal, startedInside = false인
// Hit다(ContainsPoint와 같은 판단). radius 0은 Capsule radius 0과 같은 선분 경로이며 시작 접촉은
// CapsuleCastStartsInside(radius 0 ContainsPoint와 같은 식)로만 정한다.
template <typename T>
CastResult<T> CastCylinder(const Vector3<T>& origin, const Vector3<T>& delta, T tMax,
                           const Cylinder<T>& cylinder) noexcept {
    const T zero = static_cast<T>(0);
    if (!(cylinder.radius > zero)) {
        const Capsule<T> segment(cylinder.start, cylinder.end, zero);
        CastResult<T> cast = CastCapsule(origin, delta, tMax, segment);
        if (cast.startedInside && !CapsuleCastStartsInside(origin, segment)) {
            // CastCapsule이 시작 접촉으로 본 점을 포함 식이 밖으로 보면 거리 0 접촉으로 낸다.
            cast = CastHit(zero, origin, OpposingDirection(delta));
        }
        return cast;
    }

    const Vector3<T> axis = CylinderUnitAxis(cylinder);
    const CylinderCoordinates<T> at = ToCylinderCoordinates(cylinder, axis, origin);
    const T radiusSquared = cylinder.radius * cylinder.radius;
    // 옆면 안·밖은 포함 식과 같은 비교로 정하고 2차식에는 그 차이 값을 쓴다. 밖이면 두 값이 서로
    // 다른 유한값이라 차이는 0보다 크다(IEEE 뺄셈은 서로 다른 유한값에서 0이 되지 않는다).
    const bool radialOutside = !CylinderRadialInside(at.radial, cylinder.radius);
    const T radialGap = Dot(at.radial, at.radial) - radiusSquared;
    const bool beforeStart = at.fromStart < zero;
    const bool afterEnd = at.fromEnd > zero;
    if (!beforeStart && !afterEnd && !radialOutside) {
        return CastInside(origin);
    }

    // 시작점이 밖이면 slab 밖이거나(축 속도 0이면 위에서 Miss) 무한 원기둥 밖이므로(수직 속도 0이면
    // 아래에서 Miss) 진입 후보가 적어도 하나 정해진다.
    T enter = zero;
    T leave = tMax;
    bool enterThroughCap = false;
    bool hasEnter = false;

    const T axialRate = Dot(delta, axis);
    if (axialRate == zero) {
        if (beforeStart || afterEnd) {
            return {};
        }
    } else {
        if ((beforeStart && axialRate < zero) || (afterEnd && axialRate > zero)) {
            return {};
        }
        const T toStart = -at.fromStart / axialRate;
        const T toEnd = -at.fromEnd / axialRate;
        const bool positive = axialRate > zero;
        if (beforeStart || afterEnd) {
            enter = positive ? toStart : toEnd;
            enterThroughCap = true;
            hasEnter = true;
        }
        leave = math::Min(leave, positive ? toEnd : toStart);
    }

    const Vector3<T> deltaRadial = delta - axis * axialRate;
    const T a = Dot(deltaRadial, deltaRadial);
    if (!(a > zero)) {
        if (radialOutside) {
            return {};
        }
    } else {
        const T b = Dot(at.radial, deltaRadial);
        if (radialOutside && b >= zero) {
            // 무한 원기둥 밖에서 축에서 멀어지거나(또는 수직으로 지나는) 중이다.
            return {};
        }

        const Vector3<T> closest = at.radial - deltaRadial * (b / a);
        T inner = radiusSquared - Dot(closest, closest);
        if (inner < zero) {
            if (radialOutside) {
                return {};
            }
            // 무한 원기둥 안에서 출발하면 직선은 반드시 원 안을 지난다. 반올림으로 음수가 된 값은
            // 0으로 본다.
            inner = zero;
        }

        const T root = math::Sqrt(a * inner);
        if (radialOutside) {
            const T sideEnter = radialGap / (-b + root);
            if (!hasEnter || sideEnter > enter) {
                enter = sideEnter;
                enterThroughCap = false;
                hasEnter = true;
            }
            leave = math::Min(leave, (-b + root) / a);
        } else {
            const T sideLeave = b <= zero ? (-b + root) / a : -radialGap / (b + root);
            leave = math::Min(leave, sideLeave);
        }
    }

    if (!hasEnter || enter > leave) {
        return {};
    }
    if (!(enter > zero)) {
        return CastHit(zero, origin,
                       CylinderEntryNormal(enterThroughCap, axialRate, axis, at.radial, delta));
    }

    const Vector3<T> point = origin + delta * enter;
    const Vector3<T> radialAtHit = at.radial + deltaRadial * enter;
    return CastHit(enter, point,
                   CylinderEntryNormal(enterThroughCap, axialRate, axis, radialAtHit, delta));
}

// TryBounds 반폭에 곱하는 상대 여유의 크기(machine epsilon 배수)다. 반폭 계산(차분·나눗셈·제곱·
// 합·sqrt·곱)의 상대 반올림 오차는 모두 합쳐 약 6ε이므로 8ε면 남는다(근거는 TryBounds 주석).
inline constexpr uint32 CylinderBoundsMarginEpsilons = 8;

template <typename T>
inline constexpr T CylinderBoundsRelativeMargin =
    static_cast<T>(1) + static_cast<T>(CylinderBoundsMarginEpsilons) * math::MachineEpsilon<T>;

static_assert(CylinderBoundsRelativeMargin<float> > 1.0f &&
                  CylinderBoundsRelativeMargin<double> > 1.0,
              "Cylinder bounds margin must be representable above one");

// TryBounds의 축 비율 ρ(축에 수직인 두 성분 중 큰 것 / 가장 큰 성분)의 하한이다(가장 작은 양의
// 정규 값의 배수). ρ에 곱하는 나머지 인수가 1/√3 이상이므로 2배면 곱이 정규 범위에 남아, 비정규
// 값의 절대 오차가 radius배로 커지지 않는다. 하한은 실제 ρ보다 크거나 같으므로 경계를 넓히기만
// 한다.
inline constexpr uint32 CylinderBoundsRatioFloorNormals = 2;

template <typename T>
inline constexpr T CylinderBoundsRatioFloor =
    static_cast<T>(CylinderBoundsRatioFloorNormals) * NumericLimits<T>::Min();

// cylinder 원판의 normal 방향 support 반폭 r·|n × a|다. 비단위 normal에도 정확한 support이며
// (선형 함수 n·x의 원판 위 최대 변화), √(1 - (n·a)²) 꼴과 달리 n과 a가 거의 평행해도 소거가 없다.
// radius 0은 정규화 없이 0이다.
template <typename T>
T CylinderDiscSupport(const Vector3<T>& normal, const Cylinder<T>& cylinder) noexcept {
    if (!(cylinder.radius > static_cast<T>(0))) {
        return static_cast<T>(0);
    }
    return cylinder.radius * StableLength(Cross(normal, CylinderUnitAxis(cylinder)));
}

} // namespace detail

} // namespace rocks
