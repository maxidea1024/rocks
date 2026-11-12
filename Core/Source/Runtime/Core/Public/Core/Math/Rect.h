#pragma once

#include <Core/Containers/Span.h>
#include <Core/Math/IntVector.h>
#include <Core/Math/Vector2.h>

namespace rocks {

// 2차원 축 정렬 경계(UE FBox2D 대응)다. 닫힌 집합 [min, max]이며 경계 접촉은 겹침이다.
// 유효 조건: 모든 성분이 유한하고 축마다 min <= max. 넓이 0인 선분·점 경계도 유효하다.
// UE FBox2D의 bIsValid처럼 "아직 비어 있음" 상태를 두지 않는다. 점 집합으로 만들 때는
// TryFromPoints를 사용하고, 빈 입력은 실패로 반환한다.
template <typename T>
struct Box2 {
    static_assert(math::IsFloatingPointV<T>, "Box2 supports float and double");

    Vector2<T> min;
    Vector2<T> max;

    constexpr Box2() noexcept = default;

    constexpr Box2(const Vector2<T>& minimum, const Vector2<T>& maximum) noexcept
        : min(minimum), max(maximum) {
    }

    // 실패(InvalidArgument): 비유한 성분 또는 축별 minimum > maximum.
    static Result<Box2> TryMake(const Vector2<T>& minimum, const Vector2<T>& maximum) noexcept {
        const Box2 box(minimum, maximum);
        if (!box.IsValid()) {
            return Result<Box2>::Failure(ErrorCode::InvalidArgument);
        }
        return Result<Box2>::Success(box);
    }

    // 점들을 모두 포함하는 최소 경계다. 실패(InvalidArgument): 빈 입력, 비유한 점.
    static Result<Box2> TryFromPoints(Span<const Vector2<T>> points) noexcept {
        if (points.Size() == 0) {
            return Result<Box2>::Failure(ErrorCode::InvalidArgument);
        }
        Box2 box(points[0], points[0]);
        for (usize index = 0; index < points.Size(); ++index) {
            if (!points[index].IsFinite()) {
                return Result<Box2>::Failure(ErrorCode::InvalidArgument);
            }
            box.min = ComponentMin(box.min, points[index]);
            box.max = ComponentMax(box.max, points[index]);
        }
        return Result<Box2>::Success(box);
    }

    friend constexpr bool operator==(const Box2&, const Box2&) noexcept = default;

    constexpr bool IsValid() const noexcept {
        return min.IsFinite() && max.IsFinite() && min.x <= max.x && min.y <= max.y;
    }

    constexpr Vector2<T> Size() const noexcept {
        return max - min;
    }

    constexpr Vector2<T> Center() const noexcept {
        return (min + max) * static_cast<T>(0.5);
    }

    // 중심에서 각 변까지의 반 크기다(UE GetExtent).
    constexpr Vector2<T> Extent() const noexcept {
        return Size() * static_cast<T>(0.5);
    }

    constexpr T Area() const noexcept {
        const Vector2<T> size = Size();
        return size.x * size.y;
    }

    // 경계 포함 판정이다(UE IsInsideOrOn). NaN 점은 false다.
    constexpr bool Contains(const Vector2<T>& point) const noexcept {
        return point.x >= min.x && point.x <= max.x && point.y >= min.y && point.y <= max.y;
    }

    // 경계 제외 판정이다(UE IsInside).
    constexpr bool ContainsStrictly(const Vector2<T>& point) const noexcept {
        return point.x > min.x && point.x < max.x && point.y > min.y && point.y < max.y;
    }

    // other 전체가 이 경계 안(경계 포함)이면 true다. other가 유효하지 않으면(뒤집힘·NaN) false다.
    constexpr bool Contains(const Box2& other) const noexcept {
        return other.IsValid() && Contains(other.min) && Contains(other.max);
    }

    // 닫힌 경계끼리 겹침이다. 변 접촉도 true다(UE Intersect와 같다).
    constexpr bool Overlaps(const Box2& other) const noexcept {
        return min.x <= other.max.x && other.min.x <= max.x && min.y <= other.max.y &&
               other.min.y <= max.y;
    }

    // 두 경계의 교집합이다. 어느 쪽이든 유효하지 않으면 InvalidArgument, 겹치지 않으면 NotFound다
    // (UE Overlap은 무효 박스를 반환한다).
    Result<Box2> TryIntersection(const Box2& other) const noexcept {
        if (!IsValid() || !other.IsValid()) {
            return Result<Box2>::Failure(ErrorCode::InvalidArgument);
        }
        if (!Overlaps(other)) {
            return Result<Box2>::Failure(ErrorCode::NotFound);
        }
        return Result<Box2>::Success(
            Box2(ComponentMax(min, other.min), ComponentMin(max, other.max)));
    }

    // 합집합 경계다. NaN 성분은 결과에 그대로 남아 IsValid가 false가 된다(누적 중 사라지지 않음).
    constexpr Box2 Union(const Box2& other) const noexcept {
        return Box2(MinVector(min, other.min), MaxVector(max, other.max));
    }

    constexpr Box2 ExpandedToInclude(const Vector2<T>& point) const noexcept {
        return Box2(MinVector(min, point), MaxVector(max, point));
    }

    // 각 변을 amount만큼 바깥으로 늘린다. 음수는 줄이며 결과가 뒤집히면 IsValid가 false다.
    constexpr Box2 ExpandedBy(const Vector2<T>& amount) const noexcept {
        return Box2(min - amount, max + amount);
    }

    // 경계 위·안의 가장 가까운 점이다. 유효한 경계가 전제이며 point의 NaN 성분은 NaN으로 남는다.
    constexpr Vector2<T> ClosestPoint(const Vector2<T>& point) const noexcept {
        return Vector2<T>(ClampKeepNaN(point.x, min.x, max.x), ClampKeepNaN(point.y, min.y, max.y));
    }

private:
    static constexpr Vector2<T> MinVector(const Vector2<T>& a, const Vector2<T>& b) noexcept {
        return Vector2<T>(math::MinPropagateNaN(a.x, b.x), math::MinPropagateNaN(a.y, b.y));
    }

    static constexpr Vector2<T> MaxVector(const Vector2<T>& a, const Vector2<T>& b) noexcept {
        return Vector2<T>(math::MaxPropagateNaN(a.x, b.x), math::MaxPropagateNaN(a.y, b.y));
    }

    static constexpr T ClampKeepNaN(T value, T minimum, T maximum) noexcept {
        if (value < minimum) {
            return minimum;
        }
        if (value > maximum) {
            return maximum;
        }
        return value;
    }
};

// 정수 픽셀·셀 사각형(UE FIntRect 대응)이다. 반열린 구간 [min, max)이며 max는 포함하지 않는다.
// 너비·높이는 max - min이고, 빈 사각형(넓이 0)도 유효하다. Size의 int32 overflow는 Fatal이다.
struct IntRect {
    IntVec2 min;
    IntVec2 max;

    constexpr IntRect() noexcept = default;

    constexpr IntRect(const IntVec2& minimum, const IntVec2& maximum) noexcept
        : min(minimum), max(maximum) {
    }

    friend constexpr bool operator==(const IntRect&, const IntRect&) noexcept = default;

    constexpr bool IsValid() const noexcept {
        return min.x <= max.x && min.y <= max.y;
    }

    constexpr bool IsEmpty() const noexcept {
        return min.x >= max.x || min.y >= max.y;
    }

    IntVec2 Size() const noexcept {
        return max - min;
    }

    // 넓이다. 유효한 사각형의 너비·높이는 각각 2^32 - 1 이하이므로 곱은 uint64에 항상 들어간다.
    // 유효하지 않은 사각형은 Fatal이다.
    uint64 Area() const noexcept {
        if (!IsValid()) {
            platform::Fatal("IntRect area requires min <= max");
        }
        const auto width = static_cast<uint64>(static_cast<int64>(max.x) - min.x);
        const auto height = static_cast<uint64>(static_cast<int64>(max.y) - min.y);
        return width * height;
    }

    // min <= point < max (UE FIntRect::Contains와 같은 반열린 판정).
    constexpr bool Contains(const IntVec2& point) const noexcept {
        return point.x >= min.x && point.x < max.x && point.y >= min.y && point.y < max.y;
    }

    // 넓이가 있는 공통 영역이 있으면 true다. 변만 닿는 경우와 어느 한쪽이 비었거나 뒤집힌 경우는
    // 겹침이 아니다(UE FIntRect::Intersect는 빈 사각형도 true일 수 있다).
    constexpr bool Overlaps(const IntRect& other) const noexcept {
        if (IsEmpty() || other.IsEmpty()) {
            return false;
        }
        return min.x < other.max.x && other.min.x < max.x && min.y < other.max.y &&
               other.min.y < max.y;
    }
};

} // namespace rocks
