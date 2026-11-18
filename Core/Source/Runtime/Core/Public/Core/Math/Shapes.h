#pragma once

#include <Core/Containers/Span.h>
#include <Core/Math/Quaternion.h>
#include <Platform/PlatformMath.h>

namespace rocks {

// 기본 기하 shape 값 타입이다. 교차·포함·거리 질의는 Core/Math/Intersection.h에 있다.
//
// 공통 계약
// - 좌표계는 UE와 같은 왼손 +X 전방, +Y 오른쪽, +Z 위이며 길이는 cm다(MathUtility.h).
//   T는 float 또는 double이다.
// - Sphere, Box, Capsule, OrientedBox는 경계를 포함하는 닫힌 solid다. 경계에 닿기만 해도
//   포함·겹침이다. Plane, Triangle, Segment는 두께가 없는 점 집합이다.
// - 필드는 공개되어 있고 값 생성자는 검사하지 않는다. 외부 입력은 TryMake 계열로 검증·정규화하거나
//   IsValid로 확인한다. 질의 함수는 매 호출마다 IsValid를 확인하여 잘못된 입력을 접촉 결과와
//   구분한다.
// - 단위 벡터·단위 quaternion 판정은 길이 절대 오차 ShapeUnitTolerance(1e-5)까지 허용한다. sqrt를
//   피하려고 |길이² - 1| <= 2e-5로 검사한다. TryMake 계열이 만든 값은 반올림 수준으로 이 안에 있다.
// - 계산은 T 정밀도로만 수행하고 숨은 double 승격은 없다. 일부 질의는 좌표의 4제곱 크기 중간값을
//   만들므로 좌표·반지름 절댓값은 float 약 1e9, double 약 1e75 이하를 전제한다. 이 범위를 넘는
//   유한값은 검사하지 않으며 결과를 보장하지 않는다.
// - float↔double 변환은 명시적 생성자로만 허용한다. float 단위 값을 double로 바꿔도 허용 오차 안에
//   남는다.
// - UE와 달리 기본 생성값은 모두 유효한 shape다. 빈 Box는 Box::Empty()로만 만든다.
// - 비균일 scale·shear가 있는 변환을 shape에 적용하는 기능은 Transform 단계의 별도 범위다.
template <typename T>
    requires math::IsFloatingPointV<T>
inline constexpr T ShapeUnitTolerance = static_cast<T>(1.0e-5);

namespace detail {

// 템플릿 인자 추론에서 제외할 scalar 매개변수용이다. Raycast(rayF, sphereF, 10.0)처럼 double
// literal을 넘겨도 T는 shape에서만 추론되고 인자는 T로 변환된다.
template <typename T>
struct GeometryNonDeduced {
    using Type = T;
};

template <typename T>
using GeometryScalar = typename GeometryNonDeduced<T>::Type;

template <typename T>
constexpr T GeometryInfinity() noexcept {
    if constexpr (IsSameV<T, float>) {
        return platform::PlatformMath::Infinity<float>();
    } else {
        return platform::PlatformMath::Infinity<double>();
    }
}

// 길이 오차 약 ShapeUnitTolerance 이내인지 제곱 길이로 판정한다. NaN·Inf는 false다.
template <typename T>
constexpr bool IsShapeUnitLengthSquared(T lengthSquared) noexcept {
    const T error = math::Abs(lengthSquared - static_cast<T>(1));
    return error <= static_cast<T>(2) * ShapeUnitTolerance<T>;
}

template <typename T>
constexpr bool IsShapeUnitVector(const Vector3<T>& vector) noexcept {
    return vector.IsFinite() && IsShapeUnitLengthSquared(vector.LengthSquared());
}

template <typename T>
constexpr bool IsShapeUnitQuaternion(const Quaternion<T>& rotation) noexcept {
    return rotation.IsFinite() && IsShapeUnitLengthSquared(rotation.LengthSquared());
}

template <typename T>
constexpr bool IsNonNegativeFinite(T value) noexcept {
    return math::IsFinite(value) && value >= static_cast<T>(0);
}

} // namespace detail

// 평면 {p | Dot(normal, p) == distance}다. normal은 단위 벡터이고 distance는 원점에서 normal
// 방향으로 잰 부호 있는 거리다(UE FPlane의 W와 같은 의미). SignedDistance가 양수인 쪽이 앞면이다.
// 기본값은 normal=+Z, distance=0인 지면(z=0) 평면이다.
template <typename T>
struct Plane {
    static_assert(math::IsFloatingPointV<T>, "Plane supports float and double");

    Vector3<T> normal = Vector3<T>::Up();
    T distance = static_cast<T>(0);

    constexpr Plane() noexcept = default;

    // 검사하지 않는 값 생성자다. unitNormal이 단위 벡터가 아니면 IsValid가 false다.
    constexpr Plane(const Vector3<T>& unitNormal, T distanceValue) noexcept
        : normal(unitNormal), distance(distanceValue) {
    }

    template <typename U>
        requires(math::IsFloatingPointV<U> && !IsSameV<U, T>)
    explicit constexpr Plane(const Plane<U>& other) noexcept
        : normal(other.normal), distance(static_cast<T>(other.distance)) {
    }

    // Dot(normalValue, p) == distanceValue인 평면을 단위 normal 형태로 만든다. 같은 점 집합을
    // 유지하도록 distance도 |normalValue|로 나눈다. 실패(InvalidArgument): 유한하지 않은 입력,
    // 영벡터 normal, 나눈 distance가 유한하지 않음.
    static Result<Plane> TryMake(const Vector3<T>& normalValue, T distanceValue) noexcept {
        auto unit = normalValue.TryNormalize();
        if (!unit || !math::IsFinite(distanceValue)) {
            return Result<Plane>::Failure(ErrorCode::InvalidArgument);
        }

        // TryNormalize와 같이 가장 큰 성분으로 먼저 나눠 |normalValue| 계산의 overflow를 피한다.
        const T largest = normalValue.MaxAbsComponent();
        const T scaledLength = (normalValue / largest).Length();
        const T unitDistance = distanceValue / largest / scaledLength;
        if (!math::IsFinite(unitDistance)) {
            return Result<Plane>::Failure(ErrorCode::InvalidArgument);
        }
        return Result<Plane>::Success(Plane(unit.Value(), unitDistance));
    }

    // point를 지나고 normalValue(길이 무관)에 수직인 평면이다.
    // 실패(InvalidArgument): 유한하지 않은 입력, 영벡터 normal.
    static Result<Plane> TryFromPointNormal(const Vector3<T>& point,
                                            const Vector3<T>& normalValue) noexcept {
        auto unit = normalValue.TryNormalize();
        if (!unit || !point.IsFinite()) {
            return Result<Plane>::Failure(ErrorCode::InvalidArgument);
        }

        const T pointDistance = Dot(unit.Value(), point);
        if (!math::IsFinite(pointDistance)) {
            return Result<Plane>::Failure(ErrorCode::InvalidArgument);
        }
        return Result<Plane>::Success(Plane(unit.Value(), pointDistance));
    }

    // 세 점을 지나는 평면이다. normal은 Cross(b - a, c - a) 방향으로 UE FPlane(A, B, C)
    // (UE Plane.h:441-446)·Chaos FTriangle::GetNormal(Chaos/Triangle.h:120-122)과 같은 식이다.
    // 왼손 좌표계이므로 normal 쪽에서 내려다볼 때 a → b → c는 시계 방향이다(예: 원점 → X축 점 →
    // Y축 점은 normal +Z이며 위에서 보면 전방 → 오른쪽으로 도는 시계 방향).
    // 주의: UE 렌더 메시의 앞면 normal은 반대 식 Cross(c - a, b - a)이고 화면에서 반시계로 보이는
    // 면이 앞면이다(UE MeshDescription.cpp:1311-1313, D3D12State.cpp:353 FrontCounterClockwise).
    // 렌더 메시 삼각형 (a, b, c)의 앞면 쪽을 normal로 하는 평면은 TryFromPoints(a, c, b)로 만든다.
    // distance는 a로 계산하므로 b, c의 SignedDistance는 반올림 오차만큼 0이 아닐 수 있다.
    // 실패(InvalidArgument): 유한하지 않은 입력, 일직선·중복 점(외적이 정확히 0), 외적 overflow.
    static Result<Plane> TryFromPoints(const Vector3<T>& a, const Vector3<T>& b,
                                       const Vector3<T>& c) noexcept {
        if (!a.IsFinite() || !b.IsFinite() || !c.IsFinite()) {
            return Result<Plane>::Failure(ErrorCode::InvalidArgument);
        }
        return TryFromPointNormal(a, Cross(b - a, c - a));
    }

    // normal이 유한한 단위 벡터(ShapeUnitTolerance)이고 distance가 유한하면 true다.
    constexpr bool IsValid() const noexcept {
        return detail::IsShapeUnitVector(normal) && math::IsFinite(distance);
    }

    // Dot(normal, point) - distance다. 앞면(normal 쪽)이 양수이며 normal이 단위 벡터일 때 실제
    // 거리다.
    constexpr T SignedDistance(const Vector3<T>& point) const noexcept {
        return Dot(normal, point) - distance;
    }

    // point를 평면에 수직 투영한다. 결과의 SignedDistance는 반올림 오차만큼 0이 아닐 수 있다.
    constexpr Vector3<T> ProjectPoint(const Vector3<T>& point) const noexcept {
        return point - normal * SignedDistance(point);
    }

    // 같은 점 집합에서 앞뒤만 바꾼 평면이다.
    constexpr Plane Flipped() const noexcept {
        return Plane(-normal, -distance);
    }
};

// 반직선 {origin + direction * s | s >= 0}이다. direction은 단위 벡터여야 하므로 s는 실제
// 거리(cm)다. 기본값은 UE TRay 기본 생성자·Init과 같이 원점에서 +Z(위)를 향한다(UE Ray.h:30-34,
// :66-72).
template <typename T>
struct Ray {
    static_assert(math::IsFloatingPointV<T>, "Ray supports float and double");

    Vector3<T> origin;
    Vector3<T> direction = Vector3<T>::Up();

    constexpr Ray() noexcept = default;

    // 검사하지 않는 값 생성자다. unitDirection이 단위 벡터가 아니면 IsValid가 false다.
    constexpr Ray(const Vector3<T>& originValue, const Vector3<T>& unitDirection) noexcept
        : origin(originValue), direction(unitDirection) {
    }

    template <typename U>
        requires(math::IsFloatingPointV<U> && !IsSameV<U, T>)
    explicit constexpr Ray(const Ray<U>& other) noexcept
        : origin(other.origin), direction(other.direction) {
    }

    // directionValue를 정규화한다. UE TRay 생성자처럼 정규화 실패를 그냥 지나치지 않는다.
    // 실패(InvalidArgument): 유한하지 않은 입력, 영벡터 direction.
    static Result<Ray> TryMake(const Vector3<T>& originValue,
                               const Vector3<T>& directionValue) noexcept {
        auto unit = directionValue.TryNormalize();
        if (!unit || !originValue.IsFinite()) {
            return Result<Ray>::Failure(ErrorCode::InvalidArgument);
        }
        return Result<Ray>::Success(Ray(originValue, unit.Value()));
    }

    // origin이 유한하고 direction이 단위 벡터(ShapeUnitTolerance)이면 true다.
    constexpr bool IsValid() const noexcept {
        return origin.IsFinite() && detail::IsShapeUnitVector(direction);
    }

    // origin + direction * distanceValue다. 음수 거리도 계산하지만 ray 위의 점은 아니다.
    constexpr Vector3<T> PointAt(T distanceValue) const noexcept {
        return origin + direction * distanceValue;
    }
};

// 선분 {Lerp(start, end, t) | 0 <= t <= 1}이다. segment 질의의 t는 거리가 아니라 이 비율이다.
// start == end인 0 길이 선분도 유효하며 점처럼 동작한다. 기본값은 원점의 0 길이 선분이다.
template <typename T>
struct Segment {
    static_assert(math::IsFloatingPointV<T>, "Segment supports float and double");

    Vector3<T> start;
    Vector3<T> end;

    constexpr Segment() noexcept = default;

    constexpr Segment(const Vector3<T>& startValue, const Vector3<T>& endValue) noexcept
        : start(startValue), end(endValue) {
    }

    template <typename U>
        requires(math::IsFloatingPointV<U> && !IsSameV<U, T>)
    explicit constexpr Segment(const Segment<U>& other) noexcept
        : start(other.start), end(other.end) {
    }

    // 실패(InvalidArgument): 유한하지 않은 끝점.
    static Result<Segment> TryMake(const Vector3<T>& startValue,
                                   const Vector3<T>& endValue) noexcept {
        if (!startValue.IsFinite() || !endValue.IsFinite()) {
            return Result<Segment>::Failure(ErrorCode::InvalidArgument);
        }
        return Result<Segment>::Success(Segment(startValue, endValue));
    }

    constexpr bool IsValid() const noexcept {
        return start.IsFinite() && end.IsFinite();
    }

    constexpr Vector3<T> Delta() const noexcept {
        return end - start;
    }

    T Length() const noexcept {
        return Delta().Length();
    }

    // t=0이면 정확히 start, t=1이면 정확히 end다.
    constexpr Vector3<T> PointAt(T t) const noexcept {
        return Lerp(start, end, t);
    }
};

// 닫힌 공 {p | |p - center| <= radius}다. radius 0은 점이며 유효하다. 음수·비유한 radius는 잘못된
// 값이다. UE TSphere와 달리 포함·겹침 판정에 기본 허용 오차를 더하지 않는다. 기본값은 원점의
// 점이다.
template <typename T>
struct Sphere {
    static_assert(math::IsFloatingPointV<T>, "Sphere supports float and double");

    Vector3<T> center;
    T radius = static_cast<T>(0);

    constexpr Sphere() noexcept = default;

    constexpr Sphere(const Vector3<T>& centerValue, T radiusValue) noexcept
        : center(centerValue), radius(radiusValue) {
    }

    template <typename U>
        requires(math::IsFloatingPointV<U> && !IsSameV<U, T>)
    explicit constexpr Sphere(const Sphere<U>& other) noexcept
        : center(other.center), radius(static_cast<T>(other.radius)) {
    }

    // 실패(InvalidArgument): 유한하지 않은 입력, 음수 radius. -0 radius는 +0으로 정규화한다.
    static Result<Sphere> TryMake(const Vector3<T>& centerValue, T radiusValue) noexcept {
        if (!centerValue.IsFinite() || !detail::IsNonNegativeFinite(radiusValue)) {
            return Result<Sphere>::Failure(ErrorCode::InvalidArgument);
        }
        return Result<Sphere>::Success(Sphere(centerValue, radiusValue + static_cast<T>(0)));
    }

    // 유한한 center와 0 이상 유한 radius면 true다. -0 radius도 유효하다.
    constexpr bool IsValid() const noexcept {
        return center.IsFinite() && detail::IsNonNegativeFinite(radius);
    }
};

// 축 정렬 닫힌 box {p | 축별 min <= p <= max}다. min == max인 축이 있는 0 부피 box(면·선·점)도
// 유효하다. 어느 축이라도 min > max이면 빈 box이며 질의 입력으로는 잘못된 값이다. UE FBox의 IsValid
// 플래그 대신 min/max 관계로 상태를 표현하며 UE 기본 생성(무효 box)과 달리 기본값은 원점의 점
// box다. min/max 이름은 Windows min/max macro와 겹치지 않도록 함수 호출 형태로 쓰지 않는다.
template <typename T>
struct Box {
    static_assert(math::IsFloatingPointV<T>, "Box supports float and double");

    Vector3<T> min;
    Vector3<T> max;

    constexpr Box() noexcept = default;

    // 검사하지 않는 값 생성자다. 자동으로 정렬하지 않으므로 역전된 입력은 IsValid가 false다.
    constexpr Box(const Vector3<T>& minimum, const Vector3<T>& maximum) noexcept
        : min{minimum}, max{maximum} {
    }

    template <typename U>
        requires(math::IsFloatingPointV<U> && !IsSameV<U, T>)
    explicit constexpr Box(const Box<U>& other) noexcept
        : min{Vector3<T>(other.min)}, max{Vector3<T>(other.max)} {
    }

    // 점을 하나씩 포함시키는 누적의 시작값이다. min=+Inf, max=-Inf이므로 IsEmpty()는 true,
    // IsValid()는 false다. ExpandedToInclude를 한 번 이상 적용하면 유효한 box가 된다.
    static constexpr Box Empty() noexcept {
        const T infinity = detail::GeometryInfinity<T>();
        return Box(Vector3<T>::Splat(infinity), Vector3<T>::Splat(-infinity));
    }

    // 실패(InvalidArgument): 유한하지 않은 입력, 어느 축이든 minimum > maximum.
    static Result<Box> TryMake(const Vector3<T>& minimum, const Vector3<T>& maximum) noexcept {
        const Box box(minimum, maximum);
        if (!box.IsValid()) {
            return Result<Box>::Failure(ErrorCode::InvalidArgument);
        }
        return Result<Box>::Success(box);
    }

    // center ± halfExtents box다. 실패(InvalidArgument): 유한하지 않은 입력, 음수 halfExtents 성분,
    // 모서리 좌표 overflow.
    static Result<Box> TryFromCenterHalfExtents(const Vector3<T>& center,
                                                const Vector3<T>& halfExtents) noexcept {
        if (!center.IsFinite() || !detail::IsNonNegativeFinite(halfExtents.x) ||
            !detail::IsNonNegativeFinite(halfExtents.y) ||
            !detail::IsNonNegativeFinite(halfExtents.z)) {
            return Result<Box>::Failure(ErrorCode::InvalidArgument);
        }
        return TryMake(center - halfExtents, center + halfExtents);
    }

    // 모든 점을 포함하는 가장 작은 box다. 실패(InvalidArgument): 빈 span, 유한하지 않은 점.
    static Result<Box> TryFromPoints(Span<const Vector3<T>> points) noexcept {
        if (points.Size() == 0) {
            return Result<Box>::Failure(ErrorCode::InvalidArgument);
        }

        Box box = Empty();
        for (usize index = 0; index < points.Size(); ++index) {
            const Vector3<T>& point = points[index];
            if (!point.IsFinite()) {
                return Result<Box>::Failure(ErrorCode::InvalidArgument);
            }
            box = box.ExpandedToInclude(point);
        }
        return Result<Box>::Success(box);
    }

    // 모든 성분이 유한하고 축별 min <= max면 true다.
    constexpr bool IsValid() const noexcept {
        return min.IsFinite() && max.IsFinite() && min.x <= max.x && min.y <= max.y &&
               min.z <= max.z;
    }

    // 어느 축이든 min > max면 true다. NaN 성분은 비교가 false이므로 비었다고 보지
    // 않는다(IsValid도 false).
    constexpr bool IsEmpty() const noexcept {
        return min.x > max.x || min.y > max.y || min.z > max.z;
    }

    // 각 끝을 먼저 절반으로 만들어 큰 유한 좌표에서도 합이 overflow하지 않는다.
    constexpr Vector3<T> Center() const noexcept {
        const T half = static_cast<T>(0.5);
        return min * half + max * half;
    }

    // 축별 절반 크기(UE Extent와 같은 의미)다.
    constexpr Vector3<T> HalfExtents() const noexcept {
        const T half = static_cast<T>(0.5);
        return max * half - min * half;
    }

    constexpr Vector3<T> Size() const noexcept {
        return max - min;
    }

    // point를 포함하도록 넓힌 box다. point나 box에 NaN이 있으면 결과도 그 성분이 NaN이 되어
    // IsValid가 false다(뒤이은 확장에서도 NaN이 사라지지 않는다).
    constexpr Box ExpandedToInclude(const Vector3<T>& point) const noexcept {
        return Box(
            Vector3<T>(math::MinPropagateNaN(min.x, point.x), math::MinPropagateNaN(min.y, point.y),
                       math::MinPropagateNaN(min.z, point.z)),
            Vector3<T>(math::MaxPropagateNaN(max.x, point.x), math::MaxPropagateNaN(max.y, point.y),
                       math::MaxPropagateNaN(max.z, point.z)));
    }

    // 경계 포함 판정이다(Box2::Contains, UE FBox::IsInsideOrOn과 같다). NaN 성분이 있는 점은
    // false다. 입력 오류를 결과와 구분해야 하면 Intersection.h의 ContainsPoint를 쓴다.
    constexpr bool Contains(const Vector3<T>& point) const noexcept {
        return point.x >= min.x && point.x <= max.x && point.y >= min.y && point.y <= max.y &&
               point.z >= min.z && point.z <= max.z;
    }

    // other 전체가 이 box 안(닫힌 경계 포함)이면 true다(Box2::Contains(Box2)와 같은 규칙). other가
    // 유효하지 않으면(역전·NaN·무한대) false다. 이 box가 NaN을 가지면 비교가 거짓이라 false다.
    // 크기 0 box(점·면)도 경계 위에 있으면 포함이다.
    //
    // 사용 예(느슨한 노드 경계에 프록시가 들어가는지):
    //   if (looseBounds.Contains(proxyBounds)) { ... }
    constexpr bool Contains(const Box& other) const noexcept {
        return other.IsValid() && other.min.x >= min.x && other.max.x <= max.x &&
               other.min.y >= min.y && other.max.y <= max.y && other.min.z >= min.z &&
               other.max.z <= max.z;
    }

    // 두 box를 모두 포함하는 가장 작은 box다(Box2::Union과 같은 규칙). NaN 성분은 결과에 그대로
    // 남아 IsValid가 false다. Empty()와의 합집합은 다른 쪽 그대로다.
    constexpr Box Union(const Box& other) const noexcept {
        return Box(Vector3<T>(math::MinPropagateNaN(min.x, other.min.x),
                              math::MinPropagateNaN(min.y, other.min.y),
                              math::MinPropagateNaN(min.z, other.min.z)),
                   Vector3<T>(math::MaxPropagateNaN(max.x, other.max.x),
                              math::MaxPropagateNaN(max.y, other.max.y),
                              math::MaxPropagateNaN(max.z, other.max.z)));
    }

    // 각 면을 축별 amount만큼 바깥으로 옮긴 box다(Box2::ExpandedBy, UE FBox::ExpandBy와 같다).
    // 음수는 줄이며 결과가 역전되면 IsValid가 false다. 계산은 T 반올림을 거치므로 결과가 수학적
    // 값보다 1 ULP 안쪽일 수 있다. 반올림까지 바깥쪽이어야 하면
    // ToFloatOutward·math::NextDown/NextUp으로 보정한다.
    constexpr Box ExpandedBy(const Vector3<T>& amount) const noexcept {
        return Box(min - amount, max + amount);
    }

    // double box를 덮는 float box다. min은 아래쪽(math::ToFloatRoundedDown), max는 위쪽
    // (math::ToFloatRoundedUp)으로 방향 반올림하므로 결과는 원래 box를 항상 포함한다(수학적으로,
    // 비트 단위 비교로도). float 범위를 넘는 성분은 무한대 또는 float 최대값이 되어 포함 관계는
    // 유지하지만 IsValid는 false일 수 있다. NaN 성분은 NaN으로 남는다.
    //
    // 사용 예(노드 중심 기준 상대 좌표를 float로 저장, 한 ULP 더 바깥쪽):
    //   const Box<float> relative = Box<double>(bounds.min - center, bounds.max - center)
    //                                   .ToFloatOutward();
    //   const float storedMin = math::NextDown(relative.min.x);
    constexpr Box<float> ToFloatOutward() const noexcept
        requires IsSameV<T, double>
    {
        return Box<float>(
            Vector3<float>(math::ToFloatRoundedDown(min.x), math::ToFloatRoundedDown(min.y),
                           math::ToFloatRoundedDown(min.z)),
            Vector3<float>(math::ToFloatRoundedUp(max.x), math::ToFloatRoundedUp(max.y),
                           math::ToFloatRoundedUp(max.z)));
    }
};

// 꼭짓점 a, b, c로 이루어진 닫힌 삼각형(두께 없는 면)이다. 기하 앞면 normal은 Cross(b - a, c - a)
// 방향으로 UE FPlane(A, B, C)·Chaos FTriangle::GetNormal과 같은 식이며(Plane::TryFromPoints 참고),
// 왼손 좌표계에서 앞면 쪽에서 볼 때 a → b → c가 시계 방향이다. 이 "앞면"은 UE 렌더 메시의 앞면
// (normal Cross(c - a, b - a), 화면 반시계 — UE MeshDescription.cpp:1311-1313)과 반대다. 렌더 메시
// 삼각형 (a, b, c)를 FrontOnly로 판정하려면 Triangle(a, c, b)로 넘긴다. UE도 정적 메시 충돌을
// 만들 때 bFlipNormals로 두 꼭짓점을 바꿔 Chaos 규약(왼손에서 시계)으로 옮긴다(StaticMesh.cpp:9343,
// ChaosCooking.cpp:38-42). 넓이가 0인(외적이 정확히 0인) 퇴화 삼각형도 유효한 값이다. 퇴화
// 삼각형은 최근접점·sphere 겹침에서는 선분·점으로 동작하지만 ray·segment 교차에서는 항상 Miss다.
template <typename T>
struct Triangle {
    static_assert(math::IsFloatingPointV<T>, "Triangle supports float and double");

    Vector3<T> a;
    Vector3<T> b;
    Vector3<T> c;

    constexpr Triangle() noexcept = default;

    constexpr Triangle(const Vector3<T>& aValue, const Vector3<T>& bValue,
                       const Vector3<T>& cValue) noexcept
        : a(aValue), b(bValue), c(cValue) {
    }

    template <typename U>
        requires(math::IsFloatingPointV<U> && !IsSameV<U, T>)
    explicit constexpr Triangle(const Triangle<U>& other) noexcept
        : a(other.a), b(other.b), c(other.c) {
    }

    // 실패(InvalidArgument): 유한하지 않은 꼭짓점. 퇴화 삼각형은 실패가
    // 아니다(IsDegenerate로 구분).
    static Result<Triangle> TryMake(const Vector3<T>& aValue, const Vector3<T>& bValue,
                                    const Vector3<T>& cValue) noexcept {
        const Triangle triangle(aValue, bValue, cValue);
        if (!triangle.IsValid()) {
            return Result<Triangle>::Failure(ErrorCode::InvalidArgument);
        }
        return Result<Triangle>::Success(triangle);
    }

    constexpr bool IsValid() const noexcept {
        return a.IsFinite() && b.IsFinite() && c.IsFinite();
    }

    // Cross(b - a, c - a)의 모든 성분이 정확히 0이면 true다. 허용 오차가 없으므로 거의 일직선인
    // sliver는 퇴화로 보지 않는다.
    constexpr bool IsDegenerate() const noexcept {
        return Cross(b - a, c - a) == Vector3<T>::Zero();
    }

    // 단위 앞면 normal이다. 실패(InvalidArgument): 퇴화 삼각형, 유한하지 않은 외적.
    Result<Vector3<T>> TryNormal() const noexcept {
        return Cross(b - a, c - a).TryNormalize();
    }
};

// 선분 start–end에서 거리 radius 이하인 닫힌 점 집합(양 끝 반구 포함)이다. start == end이면
// sphere와 같다. UE FCapsuleShape(center·orientation·length와 기본 허용 오차)와 달리 축 끝점 두
// 개로 표현하며 허용 오차가 없다. 기본값은 원점의 점이다.
template <typename T>
struct Capsule {
    static_assert(math::IsFloatingPointV<T>, "Capsule supports float and double");

    Vector3<T> start;
    Vector3<T> end;
    T radius = static_cast<T>(0);

    constexpr Capsule() noexcept = default;

    constexpr Capsule(const Vector3<T>& startValue, const Vector3<T>& endValue,
                      T radiusValue) noexcept
        : start(startValue), end(endValue), radius(radiusValue) {
    }

    template <typename U>
        requires(math::IsFloatingPointV<U> && !IsSameV<U, T>)
    explicit constexpr Capsule(const Capsule<U>& other) noexcept
        : start(other.start), end(other.end), radius(static_cast<T>(other.radius)) {
    }

    // 실패(InvalidArgument): 유한하지 않은 입력, 음수 radius. -0 radius는 +0으로 정규화한다.
    static Result<Capsule> TryMake(const Vector3<T>& startValue, const Vector3<T>& endValue,
                                   T radiusValue) noexcept {
        if (!startValue.IsFinite() || !endValue.IsFinite() ||
            !detail::IsNonNegativeFinite(radiusValue)) {
            return Result<Capsule>::Failure(ErrorCode::InvalidArgument);
        }
        return Result<Capsule>::Success(
            Capsule(startValue, endValue, radiusValue + static_cast<T>(0)));
    }

    constexpr bool IsValid() const noexcept {
        return start.IsFinite() && end.IsFinite() && detail::IsNonNegativeFinite(radius);
    }

    // 중심축 선분이다.
    constexpr Segment<T> Axis() const noexcept {
        return Segment<T>(start, end);
    }
};

// 회전된 닫힌 box다. local 좌표 q가 축별 |q| <= halfExtents를 만족하는 점 center + rotation(q)의
// 집합이다. rotation은 단위 quaternion, halfExtents는 0 이상(0 부피 허용)이어야 한다. UE
// FOrientedBox(단위 축 벡터 3개와 extent, 기본 extent 1)와 달리 회전을 quaternion으로 저장하며
// 기본값은 원점의 점 box다. local↔world 변환은 회전 반올림을 거치므로 회전된 box의 경계 판정은
// 정확하지 않다.
template <typename T>
struct OrientedBox {
    static_assert(math::IsFloatingPointV<T>, "OrientedBox supports float and double");

    Vector3<T> center;
    Quaternion<T> rotation;
    Vector3<T> halfExtents;

    constexpr OrientedBox() noexcept = default;

    // 검사하지 않는 값 생성자다. unitRotation이 단위 quaternion이 아니면 IsValid가 false다.
    constexpr OrientedBox(const Vector3<T>& centerValue, const Quaternion<T>& unitRotation,
                          const Vector3<T>& halfExtentsValue) noexcept
        : center(centerValue), rotation(unitRotation), halfExtents(halfExtentsValue) {
    }

    template <typename U>
        requires(math::IsFloatingPointV<U> && !IsSameV<U, T>)
    explicit constexpr OrientedBox(const OrientedBox<U>& other) noexcept
        : center(other.center), rotation(other.rotation), halfExtents(other.halfExtents) {
    }

    // rotationValue를 정규화한다. 실패(InvalidArgument): 유한하지 않은 입력, 영 quaternion,
    // 음수 halfExtents 성분.
    static Result<OrientedBox> TryMake(const Vector3<T>& centerValue,
                                       const Quaternion<T>& rotationValue,
                                       const Vector3<T>& halfExtentsValue) noexcept {
        auto unit = rotationValue.TryNormalize();
        if (!unit || !centerValue.IsFinite() || !detail::IsNonNegativeFinite(halfExtentsValue.x) ||
            !detail::IsNonNegativeFinite(halfExtentsValue.y) ||
            !detail::IsNonNegativeFinite(halfExtentsValue.z)) {
            return Result<OrientedBox>::Failure(ErrorCode::InvalidArgument);
        }
        return Result<OrientedBox>::Success(
            OrientedBox(centerValue, unit.Value(), halfExtentsValue));
    }

    // 유한한 center, 0 이상 유한 halfExtents, 단위 rotation(ShapeUnitTolerance)이면 true다.
    constexpr bool IsValid() const noexcept {
        return center.IsFinite() && detail::IsNonNegativeFinite(halfExtents.x) &&
               detail::IsNonNegativeFinite(halfExtents.y) &&
               detail::IsNonNegativeFinite(halfExtents.z) &&
               detail::IsShapeUnitQuaternion(rotation);
    }

    // 계산용 회전이다. IsValid는 길이 오차 ShapeUnitTolerance까지 허용하지만 RotateVector는 정확한
    // 단위 quaternion을 전제하므로(길이 1 + e이면 벡터가 약 2e 비율로 늘어난다) 축·좌표 변환은 모두
    // 이 정규화 결과를 쓴다. 정규화할 수 없는(유한하지 않음·영) 회전이면 저장값을 그대로 돌려주며
    // 이때 box는 IsValid가 false이므로 결과에 의미가 없다. 반복 질의에서는 한 번 구해 재사용한다.
    Quaternion<T> UnitRotation() const noexcept {
        auto unit = rotation.TryNormalize();
        return unit ? unit.Value() : rotation;
    }

    // world 좌표의 box local 축(단위 벡터, 반올림 수준)이다.
    Vector3<T> AxisX() const noexcept {
        return UnitRotation().RotateVector(Vector3<T>::UnitX());
    }

    Vector3<T> AxisY() const noexcept {
        return UnitRotation().RotateVector(Vector3<T>::UnitY());
    }

    Vector3<T> AxisZ() const noexcept {
        return UnitRotation().RotateVector(Vector3<T>::UnitZ());
    }

    // world 점을 center 기준 local 좌표로 바꾼다.
    Vector3<T> ToLocal(const Vector3<T>& worldPoint) const noexcept {
        return UnitRotation().UnrotateVector(worldPoint - center);
    }

    Vector3<T> ToWorld(const Vector3<T>& localPoint) const noexcept {
        return center + UnitRotation().RotateVector(localPoint);
    }

    // index의 bit 0/1/2가 1이면 해당 local 축(x/y/z)의 + 쪽 꼭짓점이다. index >= 8이면 Fatal이다.
    Vector3<T> Corner(uint32 index) const noexcept {
        if (index >= 8) {
            platform::Fatal("OrientedBox corner index out of range");
        }

        const Vector3<T> local((index & 1u) != 0 ? halfExtents.x : -halfExtents.x,
                               (index & 2u) != 0 ? halfExtents.y : -halfExtents.y,
                               (index & 4u) != 0 ? halfExtents.z : -halfExtents.z);
        return ToWorld(local);
    }
};

// ConvexVolume이 담을 수 있는 평면 수의 상한이다. 뷰 프러스텀(6)과 그림자 caster volume(광원
// 프러스텀에 돌출 면을 더해도 십여 개)을 넉넉히 덮는다. 고정 용량이라 heap을 쓰지 않는다.
inline constexpr uint32 ConvexVolumeMaxPlanes = 32;

// 평면들의 교집합인 닫힌 볼록 영역이다(UE FConvexVolume 대응). 각 평면의 normal은 영역 **안쪽**을
// 향한다: 점 p는 모든 평면에서 SignedDistance(p) >= 0일 때 영역 안이다(경계 포함). 평면 수는
// [1, ConvexVolumeMaxPlanes]이며 평면 사이의 관계(실제로 닫힌 영역인지, 비어 있는지)는 검사하지
// 않는다. 열린 영역(평면 하나 등)도 유효하다. 판정은 Intersection.h의
// ClassifyContainment·Overlap이다. 평면 normal은 길이 오차 ShapeUnitTolerance(1e-5)까지 단위로
// 받아들이므로 SignedDistance는 정확한 거리가 아니라 normal 길이 비율만큼 다른 근사 거리다. 영역
// 자체(SignedDistance >= 0인 반공간들의 교집합)는 그대로 정의되므로 안·밖 판정에는 영향이 없고,
// sphere 판정처럼 거리를 반지름과 비교하는 곳에서는 반지름의 약 1e-5 비율만큼 경계가 달라질 수
// 있다. 정확한 거리가 필요하면 Plane::TryMake로 정규화한 평면을 쓴다.
//
// 좌표계와 무관하다. 안쪽 normal 규약만 지키면 왼손(UE)·오른손 어느 좌표에서 만든 평면이든 같은
// 뜻이다. 프러스텀을 만들 때 평면을 바깥쪽 normal로 얻었다면 Plane::Flipped()로 뒤집어 넣는다.
//
// 사용 예:
//   const Plane<double> planes[] = {near, far, left, right, top, bottom};   // 안쪽 normal
//   Result<ConvexVolume<double>> frustum = ConvexVolume<double>::TryMake(planes);
//   if (frustum && Overlap(frustum.Value(), bounds) == QueryResult::Hit) { ... }
template <typename T>
struct ConvexVolume {
    static_assert(math::IsFloatingPointV<T>, "ConvexVolume supports float and double");

    Plane<T> planes[ConvexVolumeMaxPlanes];
    // 유효한 평면 수다. 기본값 0은 평면이 없는 잘못된 값이다(IsValid false).
    uint32 planeCount = 0;

    constexpr ConvexVolume() noexcept = default;

    template <typename U>
        requires(math::IsFloatingPointV<U> && !IsSameV<U, T>)
    explicit constexpr ConvexVolume(const ConvexVolume<U>& other) noexcept
        : planeCount(other.planeCount) {
        for (uint32 index = 0; index < ConvexVolumeMaxPlanes; ++index) {
            planes[index] = Plane<T>(other.planes[index]);
        }
    }

    // 평면들을 복사해 만든다. 실패(InvalidArgument): 평면이 없거나 ConvexVolumeMaxPlanes보다 많음,
    // 어느 평면이든 IsValid가 아님(비유한 값, 단위가 아닌 normal).
    static Result<ConvexVolume> TryMake(Span<const Plane<T>> source) noexcept {
        if (source.Size() == 0 || source.Size() > ConvexVolumeMaxPlanes) {
            return Result<ConvexVolume>::Failure(ErrorCode::InvalidArgument);
        }
        ConvexVolume volume;
        for (usize index = 0; index < source.Size(); ++index) {
            if (!source[index].IsValid()) {
                return Result<ConvexVolume>::Failure(ErrorCode::InvalidArgument);
            }
            volume.planes[index] = source[index];
        }
        volume.planeCount = static_cast<uint32>(source.Size());
        return Result<ConvexVolume>::Success(volume);
    }

    // 평면 수가 [1, ConvexVolumeMaxPlanes]이고 모든 평면이 유효하면 true다.
    constexpr bool IsValid() const noexcept {
        if (planeCount == 0 || planeCount > ConvexVolumeMaxPlanes) {
            return false;
        }
        for (uint32 index = 0; index < planeCount; ++index) {
            if (!planes[index].IsValid()) {
                return false;
            }
        }
        return true;
    }

    // 유효한 평면들이다. 이 값보다 오래 쓰지 않는다.
    Span<const Plane<T>> Planes() const noexcept {
        return Span<const Plane<T>>(planes, planeCount > ConvexVolumeMaxPlanes ? 0 : planeCount);
    }
};

} // namespace rocks
