#pragma once

#include <Core/Containers/Span.h>
#include <Core/Math/Quaternion.h>

namespace rocks {

// 기본 기하 shape 값 타입이다. 교차·포함·거리 질의는 Core/Math/Intersection.h에 있다.
//
// 공통 계약
// - 좌표계는 오른손, +X 전방, +Y 왼쪽, +Z 위이며 길이는 미터다. T는 float 또는 double이다.
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
        return __builtin_huge_valf();
    } else {
        return __builtin_huge_val();
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

    // 세 점을 지나는 평면이다. normal은 Cross(b - a, c - a) 방향이므로 normal 쪽에서 내려다볼 때
    // a → b → c가 반시계 방향이다(오른손 법칙. 예: X, Y 축 위의 점 순서는 normal +Z).
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
// 거리(미터)다. 기본값은 원점에서 +X(전방)를 향한다. UE TRay의 기본 방향은 +Z다.
template <typename T>
struct Ray {
    static_assert(math::IsFloatingPointV<T>, "Ray supports float and double");

    Vector3<T> origin;
    Vector3<T> direction = Vector3<T>::Forward();

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

    // point를 포함하도록 넓힌 box다. point에 NaN이 있으면 결과도 NaN을 포함해 IsValid가 false가
    // 된다.
    constexpr Box ExpandedToInclude(const Vector3<T>& point) const noexcept {
        return Box(ComponentMin(min, point), ComponentMax(max, point));
    }
};

// 꼭짓점 a, b, c로 이루어진 닫힌 삼각형(두께 없는 면)이다. 앞면 normal은 Cross(b - a, c - a)
// 방향이며 앞면에서 볼 때 a → b → c가 반시계 방향이다. 넓이가 0인(외적이 정확히 0인) 퇴화 삼각형도
// 유효한 값이다. 퇴화 삼각형은 최근접점·sphere 겹침에서는 선분·점으로 동작하지만 ray·segment
// 교차에서는 항상 Miss다.
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

    // world 좌표의 box local 축(단위 벡터, 반올림 수준)이다.
    Vector3<T> AxisX() const noexcept {
        return rotation.RotateVector(Vector3<T>::UnitX());
    }

    Vector3<T> AxisY() const noexcept {
        return rotation.RotateVector(Vector3<T>::UnitY());
    }

    Vector3<T> AxisZ() const noexcept {
        return rotation.RotateVector(Vector3<T>::UnitZ());
    }

    // world 점을 center 기준 local 좌표로 바꾼다.
    Vector3<T> ToLocal(const Vector3<T>& worldPoint) const noexcept {
        return rotation.UnrotateVector(worldPoint - center);
    }

    Vector3<T> ToWorld(const Vector3<T>& localPoint) const noexcept {
        return center + rotation.RotateVector(localPoint);
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

} // namespace rocks
