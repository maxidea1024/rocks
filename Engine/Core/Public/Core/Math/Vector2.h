#pragma once

#include <Core/Math/MathUtility.h>
#include <Core/Result.h>

namespace rocks {

// 2차원 벡터 값 타입이다. 화면·UV·평면 좌표처럼 축 의미는 사용하는 API가 정한다.
// 정규화·비교·특수값 규약은 Vector3와 같다. UE FVector2D와 달리 정규화 실패 tolerance를
// 제곱 길이로 받지 않고 길이 단위로 받으며, Vector3와 같은 실패 계약을 사용한다.
template <typename T>
struct Vector2 {
    static_assert(math::IsFloatingPointV<T>, "Vector2 supports float and double");

    T x = static_cast<T>(0);
    T y = static_cast<T>(0);

    constexpr Vector2() noexcept = default;

    constexpr Vector2(T xValue, T yValue) noexcept : x(xValue), y(yValue) {
    }

    template <typename U>
        requires(math::IsFloatingPointV<U> && !IsSameV<U, T>)
    explicit constexpr Vector2(const Vector2<U>& other) noexcept
        : x(static_cast<T>(other.x)), y(static_cast<T>(other.y)) {
    }

    static constexpr Vector2 Splat(T value) noexcept {
        return Vector2(value, value);
    }

    static constexpr Vector2 Zero() noexcept {
        return Vector2();
    }

    static constexpr Vector2 One() noexcept {
        return Splat(static_cast<T>(1));
    }

    static constexpr Vector2 UnitX() noexcept {
        return Vector2(static_cast<T>(1), static_cast<T>(0));
    }

    static constexpr Vector2 UnitY() noexcept {
        return Vector2(static_cast<T>(0), static_cast<T>(1));
    }

    constexpr Vector2 operator-() const noexcept {
        return Vector2(-x, -y);
    }

    constexpr Vector2 operator+(const Vector2& other) const noexcept {
        return Vector2(x + other.x, y + other.y);
    }

    constexpr Vector2 operator-(const Vector2& other) const noexcept {
        return Vector2(x - other.x, y - other.y);
    }

    constexpr Vector2 operator*(const Vector2& other) const noexcept {
        return Vector2(x * other.x, y * other.y);
    }

    constexpr Vector2 operator/(const Vector2& other) const noexcept {
        return Vector2(x / other.x, y / other.y);
    }

    constexpr Vector2 operator*(T scale) const noexcept {
        return Vector2(x * scale, y * scale);
    }

    constexpr Vector2 operator/(T divisor) const noexcept {
        return Vector2(x / divisor, y / divisor);
    }

    constexpr Vector2& operator+=(const Vector2& other) noexcept {
        *this = *this + other;
        return *this;
    }

    constexpr Vector2& operator-=(const Vector2& other) noexcept {
        *this = *this - other;
        return *this;
    }

    constexpr Vector2& operator*=(T scale) noexcept {
        *this = *this * scale;
        return *this;
    }

    friend constexpr bool operator==(const Vector2&, const Vector2&) noexcept = default;

    T Length() const noexcept {
        return math::Sqrt(LengthSquared());
    }

    constexpr T LengthSquared() const noexcept {
        const T xx = x * x;
        const T yy = y * y;
        return xx + yy;
    }

    constexpr bool IsFinite() const noexcept {
        return math::IsFinite(x) && math::IsFinite(y);
    }

    constexpr bool ContainsNaN() const noexcept {
        return math::IsNaN(x) || math::IsNaN(y);
    }

    constexpr bool IsNearlyZero(T tolerance) const noexcept {
        return math::Abs(x) <= tolerance && math::Abs(y) <= tolerance;
    }

    constexpr bool Equals(const Vector2& other, T tolerance) const noexcept {
        return math::Abs(x - other.x) <= tolerance && math::Abs(y - other.y) <= tolerance;
    }

    bool IsNormalized(T tolerance) const noexcept {
        return math::Abs(Length() - static_cast<T>(1)) <= tolerance;
    }

    constexpr T MaxAbsComponent() const noexcept {
        return math::Max(math::Abs(x), math::Abs(y));
    }

    // 실패 계약은 Vector3::TryNormalize와 같다.
    Result<Vector2> TryNormalize(T minimumLength = static_cast<T>(0)) const noexcept {
        if (!IsFinite()) {
            return Result<Vector2>::Failure(ErrorCode::InvalidArgument);
        }
        const T largest = MaxAbsComponent();
        if (largest == static_cast<T>(0)) {
            return Result<Vector2>::Failure(ErrorCode::InvalidArgument);
        }
        const Vector2 scaled = *this / largest;
        const T scaledLength = scaled.Length();
        if (!(largest * scaledLength > minimumLength)) {
            return Result<Vector2>::Failure(ErrorCode::InvalidArgument);
        }
        return Result<Vector2>::Success(scaled / scaledLength);
    }

    Vector2 NormalizedOr(const Vector2& fallback,
                         T minimumLength = static_cast<T>(0)) const noexcept {
        auto normalized = TryNormalize(minimumLength);
        return normalized ? normalized.Value() : fallback;
    }
};

using Vec2 = Vector2<float>;
using Vec2d = Vector2<double>;

static_assert(sizeof(Vec2) == 8 && sizeof(Vec2d) == 16);

template <typename T>
constexpr Vector2<T> operator*(T scale, const Vector2<T>& vector) noexcept {
    return vector * scale;
}

template <typename T>
constexpr T Dot(const Vector2<T>& a, const Vector2<T>& b) noexcept {
    const T xx = a.x * b.x;
    const T yy = a.y * b.y;
    return xx + yy;
}

// 3차원 외적의 z 성분 a.x*b.y - a.y*b.x다. 양수면 b가 a의 반시계 방향(+Z에서 내려다볼 때)이다.
template <typename T>
constexpr T Cross(const Vector2<T>& a, const Vector2<T>& b) noexcept {
    const T xy = a.x * b.y;
    const T yx = a.y * b.x;
    return xy - yx;
}

template <typename T>
T Distance(const Vector2<T>& a, const Vector2<T>& b) noexcept {
    return (b - a).Length();
}

template <typename T>
constexpr T DistanceSquared(const Vector2<T>& a, const Vector2<T>& b) noexcept {
    return (b - a).LengthSquared();
}

template <typename T>
constexpr Vector2<T> Lerp(const Vector2<T>& a, const Vector2<T>& b, T t) noexcept {
    return Vector2<T>(math::Lerp(a.x, b.x, t), math::Lerp(a.y, b.y, t));
}

template <typename T>
constexpr Vector2<T> ComponentMin(const Vector2<T>& a, const Vector2<T>& b) noexcept {
    return Vector2<T>(math::Min(a.x, b.x), math::Min(a.y, b.y));
}

template <typename T>
constexpr Vector2<T> ComponentMax(const Vector2<T>& a, const Vector2<T>& b) noexcept {
    return Vector2<T>(math::Max(a.x, b.x), math::Max(a.y, b.y));
}

} // namespace rocks
