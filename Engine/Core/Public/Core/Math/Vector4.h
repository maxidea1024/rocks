#pragma once

#include <Core/Math/Vector3.h>

namespace rocks {

// 4차원 벡터 값 타입이다. 동차 좌표(w=1 점, w=0 방향)와 4성분 데이터에 사용한다.
// float의 Dot은 platform::simd::Dot4와 같은 (p0+p1)+(p2+p3) 순서로 계산한다.
template <typename T>
struct Vector4 {
    static_assert(math::IsFloatingPointV<T>, "Vector4 supports float and double");

    T x = static_cast<T>(0);
    T y = static_cast<T>(0);
    T z = static_cast<T>(0);
    T w = static_cast<T>(0);

    constexpr Vector4() noexcept = default;

    constexpr Vector4(T xValue, T yValue, T zValue, T wValue) noexcept
        : x(xValue), y(yValue), z(zValue), w(wValue) {
    }

    constexpr Vector4(const Vector3<T>& xyz, T wValue) noexcept
        : x(xyz.x), y(xyz.y), z(xyz.z), w(wValue) {
    }

    template <typename U>
        requires(math::IsFloatingPointV<U> && !IsSameV<U, T>)
    explicit constexpr Vector4(const Vector4<U>& other) noexcept
        : x(static_cast<T>(other.x)), y(static_cast<T>(other.y)), z(static_cast<T>(other.z)),
          w(static_cast<T>(other.w)) {
    }

    static constexpr Vector4 Splat(T value) noexcept {
        return Vector4(value, value, value, value);
    }

    static constexpr Vector4 Zero() noexcept {
        return Vector4();
    }

    // 점은 w=1, 방향은 w=0이다.
    static constexpr Vector4 Point(const Vector3<T>& position) noexcept {
        return Vector4(position, static_cast<T>(1));
    }

    static constexpr Vector4 Direction(const Vector3<T>& direction) noexcept {
        return Vector4(direction, static_cast<T>(0));
    }

    constexpr Vector3<T> Xyz() const noexcept {
        return Vector3<T>(x, y, z);
    }

    constexpr Vector4 operator-() const noexcept {
        return Vector4(-x, -y, -z, -w);
    }

    constexpr Vector4 operator+(const Vector4& other) const noexcept {
        return Vector4(x + other.x, y + other.y, z + other.z, w + other.w);
    }

    constexpr Vector4 operator-(const Vector4& other) const noexcept {
        return Vector4(x - other.x, y - other.y, z - other.z, w - other.w);
    }

    constexpr Vector4 operator*(const Vector4& other) const noexcept {
        return Vector4(x * other.x, y * other.y, z * other.z, w * other.w);
    }

    constexpr Vector4 operator*(T scale) const noexcept {
        return Vector4(x * scale, y * scale, z * scale, w * scale);
    }

    constexpr Vector4 operator/(T divisor) const noexcept {
        return Vector4(x / divisor, y / divisor, z / divisor, w / divisor);
    }

    friend constexpr bool operator==(const Vector4&, const Vector4&) noexcept = default;

    // 성분 index 0..3. 범위 밖은 Fatal이다.
    constexpr T operator[](usize index) const noexcept {
        switch (index) {
        case 0:
            return x;
        case 1:
            return y;
        case 2:
            return z;
        case 3:
            return w;
        default:
            platform::Fatal("Vector4 component index out of range");
        }
    }

    constexpr bool IsFinite() const noexcept {
        return math::IsFinite(x) && math::IsFinite(y) && math::IsFinite(z) && math::IsFinite(w);
    }

    constexpr bool Equals(const Vector4& other, T tolerance) const noexcept {
        return math::Abs(x - other.x) <= tolerance && math::Abs(y - other.y) <= tolerance &&
               math::Abs(z - other.z) <= tolerance && math::Abs(w - other.w) <= tolerance;
    }
};

using Vec4 = Vector4<float>;
using Vec4d = Vector4<double>;

static_assert(sizeof(Vec4) == 16 && sizeof(Vec4d) == 32);

template <typename T>
constexpr T Dot(const Vector4<T>& a, const Vector4<T>& b) noexcept {
    const T xx = a.x * b.x;
    const T yy = a.y * b.y;
    const T zz = a.z * b.z;
    const T ww = a.w * b.w;
    const T low = xx + yy;
    const T high = zz + ww;
    return low + high;
}

template <typename T>
constexpr Vector4<T> Lerp(const Vector4<T>& a, const Vector4<T>& b, T t) noexcept {
    return Vector4<T>(math::Lerp(a.x, b.x, t), math::Lerp(a.y, b.y, t), math::Lerp(a.z, b.z, t),
                      math::Lerp(a.w, b.w, t));
}

} // namespace rocks
