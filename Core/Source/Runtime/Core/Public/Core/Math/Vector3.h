#pragma once

#include <Core/Math/MathUtility.h>
#include <Core/Result.h>

namespace rocks {

// 3차원 벡터 값 타입이다. 위치·방향·크기를 모두 표현하며 의미 구분은 사용하는 API가 정한다.
// 축 규약: 오른손 좌표계, +X 전방, +Y 왼쪽, +Z 위. Cross(UnitX, UnitY) = UnitZ다.
// Vec3(float)는 지역·렌더 계산, Vec3d(double)는 월드 위치 등 큰 좌표에 사용한다. 두 정밀도의
// 변환은 명시적 생성자로만 수행한다. 메모리 배치(x, y, z, padding 없음)를 파일·네트워크 형식으로
// 직접 쓰지 않는다.
//
// 사용 예:
//   Vec3 offset(1.0f, 0.0f, 2.0f);
//   auto direction = offset.TryNormalize();
//   if (direction) { Use(direction.Value()); }
template <typename T>
struct Vector3 {
    static_assert(math::IsFloatingPointV<T>, "Vector3 supports float and double");

    T x = static_cast<T>(0);
    T y = static_cast<T>(0);
    T z = static_cast<T>(0);

    constexpr Vector3() noexcept = default;

    constexpr Vector3(T xValue, T yValue, T zValue) noexcept : x(xValue), y(yValue), z(zValue) {
    }

    // float↔double 변환은 정밀도 손실 가능성을 드러내도록 명시적으로만 허용한다.
    template <typename U>
        requires(math::IsFloatingPointV<U> && !IsSameV<U, T>)
    explicit constexpr Vector3(const Vector3<U>& other) noexcept
        : x(static_cast<T>(other.x)), y(static_cast<T>(other.y)), z(static_cast<T>(other.z)) {
    }

    static constexpr Vector3 Splat(T value) noexcept {
        return Vector3(value, value, value);
    }

    static constexpr Vector3 Zero() noexcept {
        return Vector3();
    }

    static constexpr Vector3 One() noexcept {
        return Splat(static_cast<T>(1));
    }

    static constexpr Vector3 UnitX() noexcept {
        return Vector3(static_cast<T>(1), static_cast<T>(0), static_cast<T>(0));
    }

    static constexpr Vector3 UnitY() noexcept {
        return Vector3(static_cast<T>(0), static_cast<T>(1), static_cast<T>(0));
    }

    static constexpr Vector3 UnitZ() noexcept {
        return Vector3(static_cast<T>(0), static_cast<T>(0), static_cast<T>(1));
    }

    // Rocks 축 이름이다. UE(+Y 오른쪽, 왼손)와 Right/Left 부호가 다르므로 변환 경계에서 주의한다.
    static constexpr Vector3 Forward() noexcept {
        return UnitX();
    }

    static constexpr Vector3 Backward() noexcept {
        return -UnitX();
    }

    static constexpr Vector3 Left() noexcept {
        return UnitY();
    }

    static constexpr Vector3 Right() noexcept {
        return -UnitY();
    }

    static constexpr Vector3 Up() noexcept {
        return UnitZ();
    }

    static constexpr Vector3 Down() noexcept {
        return -UnitZ();
    }

    // ---- 성분별 산술. IEEE 규칙을 그대로 따르며 0 나눗셈은 Inf/NaN이 된다. ----
    constexpr Vector3 operator-() const noexcept {
        return Vector3(-x, -y, -z);
    }

    constexpr Vector3 operator+(const Vector3& other) const noexcept {
        return Vector3(x + other.x, y + other.y, z + other.z);
    }

    constexpr Vector3 operator-(const Vector3& other) const noexcept {
        return Vector3(x - other.x, y - other.y, z - other.z);
    }

    constexpr Vector3 operator*(const Vector3& other) const noexcept {
        return Vector3(x * other.x, y * other.y, z * other.z);
    }

    constexpr Vector3 operator/(const Vector3& other) const noexcept {
        return Vector3(x / other.x, y / other.y, z / other.z);
    }

    constexpr Vector3 operator*(T scale) const noexcept {
        return Vector3(x * scale, y * scale, z * scale);
    }

    constexpr Vector3 operator/(T divisor) const noexcept {
        return Vector3(x / divisor, y / divisor, z / divisor);
    }

    constexpr Vector3& operator+=(const Vector3& other) noexcept {
        *this = *this + other;
        return *this;
    }

    constexpr Vector3& operator-=(const Vector3& other) noexcept {
        *this = *this - other;
        return *this;
    }

    constexpr Vector3& operator*=(T scale) noexcept {
        *this = *this * scale;
        return *this;
    }

    constexpr Vector3& operator/=(T divisor) noexcept {
        *this = *this / divisor;
        return *this;
    }

    // 정확한 성분 비교다. NaN 성분이 있으면 자기 자신과도 같지 않다. 오차 비교는 Equals를 쓴다.
    friend constexpr bool operator==(const Vector3&, const Vector3&) noexcept = default;

    // 성분 index 0=x, 1=y, 2=z. 범위 밖은 Fatal이다.
    constexpr T operator[](usize index) const noexcept {
        if (index == 0) {
            return x;
        }
        if (index == 1) {
            return y;
        }
        if (index != 2) {
            platform::Fatal("Vector3 component index out of range");
        }
        return z;
    }

    // ---- 크기 ----
    // 단순 sqrt(x²+y²+z²)다. |성분|이 대략 sqrt(최댓값)을 넘으면 +Inf, 아주 작으면 0이 될 수 있다.
    // 정규화 가능 여부는 이 값 대신 TryNormalize로 판단한다.
    T Length() const noexcept {
        return math::Sqrt(LengthSquared());
    }

    constexpr T LengthSquared() const noexcept {
        const T xx = x * x;
        const T yy = y * y;
        const T xy = xx + yy;
        const T zz = z * z;
        return xy + zz;
    }

    // 모든 성분이 유한하면 true다.
    constexpr bool IsFinite() const noexcept {
        return math::IsFinite(x) && math::IsFinite(y) && math::IsFinite(z);
    }

    constexpr bool ContainsNaN() const noexcept {
        return math::IsNaN(x) || math::IsNaN(y) || math::IsNaN(z);
    }

    // 모든 성분의 |값| <= tolerance다(UE IsNearlyZero와 같은 성분별 기준).
    constexpr bool IsNearlyZero(T tolerance) const noexcept {
        return math::Abs(x) <= tolerance && math::Abs(y) <= tolerance && math::Abs(z) <= tolerance;
    }

    // 성분별 |차이| <= tolerance. NaN이 있으면 false다.
    constexpr bool Equals(const Vector3& other, T tolerance) const noexcept {
        return math::Abs(x - other.x) <= tolerance && math::Abs(y - other.y) <= tolerance &&
               math::Abs(z - other.z) <= tolerance;
    }

    // |Length() - 1| <= tolerance. tolerance는 길이 단위의 절대 오차다(제곱 길이가 아니다).
    bool IsNormalized(T tolerance) const noexcept {
        return math::Abs(Length() - static_cast<T>(1)) <= tolerance;
    }

    constexpr T MaxComponent() const noexcept {
        return math::Max(math::Max(x, y), z);
    }

    constexpr T MinComponent() const noexcept {
        return math::Min(math::Min(x, y), z);
    }

    constexpr T MaxAbsComponent() const noexcept {
        return math::Max(math::Max(math::Abs(x), math::Abs(y)), math::Abs(z));
    }

    constexpr Vector3 GetAbs() const noexcept {
        return Vector3(math::Abs(x), math::Abs(y), math::Abs(z));
    }

    // 단위 벡터를 반환한다. 실패(InvalidArgument): 유한하지 않은 성분, 영벡터,
    // 길이 <= minimumLength. minimumLength는 길이 단위의 절대값이며 기본 0은 유한한 모든
    // 비영 벡터(subnormal 포함)를 허용한다. 가장 큰 성분으로 먼저 나눠 계산하므로 제곱 길이가
    // overflow/underflow하는 입력도 정규화한다. 실패해도 이 벡터는 바뀌지 않는다.
    Result<Vector3> TryNormalize(T minimumLength = static_cast<T>(0)) const noexcept {
        if (!IsFinite()) {
            return Result<Vector3>::Failure(ErrorCode::InvalidArgument);
        }
        const T largest = MaxAbsComponent();
        if (largest == static_cast<T>(0)) {
            return Result<Vector3>::Failure(ErrorCode::InvalidArgument);
        }
        const Vector3 scaled = *this / largest;
        const T scaledLength = scaled.Length();
        // largest * scaledLength가 overflow하면 +Inf이고 minimumLength보다 크므로 허용된다.
        if (!(largest * scaledLength > minimumLength)) {
            return Result<Vector3>::Failure(ErrorCode::InvalidArgument);
        }
        return Result<Vector3>::Success(scaled / scaledLength);
    }

    // 정규화에 실패하면 호출자가 지정한 fallback을 반환한다. fallback은 검사하지 않는다.
    Vector3 NormalizedOr(const Vector3& fallback,
                         T minimumLength = static_cast<T>(0)) const noexcept {
        auto normalized = TryNormalize(minimumLength);
        return normalized ? normalized.Value() : fallback;
    }
};

using Vec3 = Vector3<float>;
using Vec3d = Vector3<double>;

static_assert(sizeof(Vec3) == 12 && sizeof(Vec3d) == 24);

template <typename T>
constexpr Vector3<T> operator*(T scale, const Vector3<T>& vector) noexcept {
    return vector * scale;
}

// (x·x' + y·y') + z·z' 순서로 누적한다. float에서는 platform 4-lane Dot3와 같은 순서다.
template <typename T>
constexpr T Dot(const Vector3<T>& a, const Vector3<T>& b) noexcept {
    const T xx = a.x * b.x;
    const T yy = a.y * b.y;
    const T xy = xx + yy;
    const T zz = a.z * b.z;
    return xy + zz;
}

// 오른손 외적이다. Cross(Forward, Left) = Up.
template <typename T>
constexpr Vector3<T> Cross(const Vector3<T>& a, const Vector3<T>& b) noexcept {
    const T yz = a.y * b.z;
    const T zy = a.z * b.y;
    const T zx = a.z * b.x;
    const T xz = a.x * b.z;
    const T xy = a.x * b.y;
    const T yx = a.y * b.x;
    return Vector3<T>(yz - zy, zx - xz, xy - yx);
}

template <typename T>
constexpr T DistanceSquared(const Vector3<T>& a, const Vector3<T>& b) noexcept {
    return (b - a).LengthSquared();
}

template <typename T>
T Distance(const Vector3<T>& a, const Vector3<T>& b) noexcept {
    return (b - a).Length();
}

// 성분별 math::Lerp다. t=0이면 a, t=1이면 b를 정확히 반환한다.
template <typename T>
constexpr Vector3<T> Lerp(const Vector3<T>& a, const Vector3<T>& b, T t) noexcept {
    return Vector3<T>(math::Lerp(a.x, b.x, t), math::Lerp(a.y, b.y, t), math::Lerp(a.z, b.z, t));
}

template <typename T>
constexpr Vector3<T> ComponentMin(const Vector3<T>& a, const Vector3<T>& b) noexcept {
    return Vector3<T>(math::Min(a.x, b.x), math::Min(a.y, b.y), math::Min(a.z, b.z));
}

template <typename T>
constexpr Vector3<T> ComponentMax(const Vector3<T>& a, const Vector3<T>& b) noexcept {
    return Vector3<T>(math::Max(a.x, b.x), math::Max(a.y, b.y), math::Max(a.z, b.z));
}

// unitNormal 방향 성분이다. unitNormal은 단위 벡터여야 하며 검사하지 않는다.
template <typename T>
constexpr Vector3<T> ProjectOntoUnit(const Vector3<T>& vector,
                                     const Vector3<T>& unitNormal) noexcept {
    return unitNormal * Dot(vector, unitNormal);
}

// 임의 길이의 onto 방향으로 사영한다. onto가 유한하지 않거나 영벡터면 InvalidArgument다.
template <typename T>
Result<Vector3<T>> TryProjectOnto(const Vector3<T>& vector, const Vector3<T>& onto) noexcept {
    auto direction = onto.TryNormalize();
    if (!direction) {
        return Result<Vector3<T>>::Failure(direction.Error());
    }
    return Result<Vector3<T>>::Success(ProjectOntoUnit(vector, direction.Value()));
}

// unitNormal 평면에 대한 반사 v - 2(v·n)n. unitNormal은 단위 벡터여야 한다.
template <typename T>
constexpr Vector3<T> Reflect(const Vector3<T>& vector, const Vector3<T>& unitNormal) noexcept {
    return vector - unitNormal * (static_cast<T>(2) * Dot(vector, unitNormal));
}

// 두 벡터 사이 각(라디안, [0, pi])이다. atan2(|a×b|, a·b)로 계산해 작은 각에서도 안정적이다.
// 어느 한쪽이 영벡터이거나 유한하지 않으면 InvalidArgument다.
template <typename T>
Result<T> TryAngleBetween(const Vector3<T>& a, const Vector3<T>& b) noexcept {
    auto unitA = a.TryNormalize();
    auto unitB = b.TryNormalize();
    if (!unitA || !unitB) {
        return Result<T>::Failure(ErrorCode::InvalidArgument);
    }
    const T sine = Cross(unitA.Value(), unitB.Value()).Length();
    const T cosine = Dot(unitA.Value(), unitB.Value());
    return Result<T>::Success(math::Atan2(sine, cosine));
}

} // namespace rocks
