#pragma once

#include <Core/Math/Matrix4.h>

namespace rocks {

// 회전 quaternion이다. 성분 순서는 (x, y, z, w)이며 w가 scalar 부분이다(UE FQuat와 같은 순서).
// 회전 방향은 오른손 법칙이다: 축을 향해 내려다볼 때 양의 각은 반시계 방향이다.
// 예: Z축 +90도 회전은 Forward(+X)를 Left(+Y)로 보낸다.
// 합성 a * b는 b를 먼저, a를 나중에 적용한다(column vector·world = parent * local과 같은 방향).
// 회전 함수는 단위 quaternion을 전제한다. 외부 입력은 TryNormalize로 검증한 뒤 사용한다.
// q와 -q는 같은 회전이지만 operator==와 Equals는 성분 비교이므로 다르다. 회전 동일성은
// AngularDistance로 판단한다. 기본값은 항등 회전이다.
template <typename T>
struct Quaternion {
    static_assert(math::IsFloatingPointV<T>, "Quaternion supports float and double");

    T x = static_cast<T>(0);
    T y = static_cast<T>(0);
    T z = static_cast<T>(0);
    T w = static_cast<T>(1);

    constexpr Quaternion() noexcept = default;

    constexpr Quaternion(T xValue, T yValue, T zValue, T wValue) noexcept
        : x(xValue), y(yValue), z(zValue), w(wValue) {
    }

    template <typename U>
        requires(math::IsFloatingPointV<U> && !IsSameV<U, T>)
    explicit constexpr Quaternion(const Quaternion<U>& other) noexcept
        : x(static_cast<T>(other.x)), y(static_cast<T>(other.y)), z(static_cast<T>(other.z)),
          w(static_cast<T>(other.w)) {
    }

    static constexpr Quaternion Identity() noexcept {
        return Quaternion();
    }

    // axis를 정규화한 뒤 radians만큼의 회전을 만든다. 실패(InvalidArgument): axis가 영벡터·
    // 유한하지 않음, radians가 유한하지 않음.
    static Result<Quaternion> TryFromAxisAngle(const Vector3<T>& axis, T radians) noexcept {
        auto unitAxis = axis.TryNormalize();
        if (!unitAxis || !math::IsFinite(radians)) {
            return Result<Quaternion>::Failure(ErrorCode::InvalidArgument);
        }
        return Result<Quaternion>::Success(FromUnitAxisAngle(unitAxis.Value(), radians));
    }

    // 단위 축 전용 빠른 생성이다. |axis| - 1의 절대 오차가 1e-3을 넘으면 Fatal이다.
    static Quaternion FromUnitAxisAngle(const Vector3<T>& unitAxis, T radians) noexcept {
        ROCKS_ASSERT(unitAxis.IsNormalized(static_cast<T>(1.0e-3)));
        const T half = radians * static_cast<T>(0.5);
        const T sine = math::Sin(half);
        return Quaternion(unitAxis.x * sine, unitAxis.y * sine, unitAxis.z * sine, math::Cos(half));
    }

    // 회전 행렬(왼쪽 위 3x3)에서 quaternion을 만든다. 직교 정규·행렬식 +1 회전 행렬이 전제이며
    // 이동·마지막 행은 무시한다. 대각합에 따라 가장 큰 성분을 먼저 구하는 방식으로 계산하여
    // 180도 근처에서도 정밀도를 유지한다. 결과는 w >= 0으로 정규화한다.
    // 실패(InvalidArgument): 유한하지 않은 원소 또는 정규화할 수 없는 결과.
    static Result<Quaternion> TryFromRotationMatrix(const Matrix4<T>& matrix) noexcept {
        const double m00 = matrix.At(0, 0);
        const double m01 = matrix.At(0, 1);
        const double m02 = matrix.At(0, 2);
        const double m10 = matrix.At(1, 0);
        const double m11 = matrix.At(1, 1);
        const double m12 = matrix.At(1, 2);
        const double m20 = matrix.At(2, 0);
        const double m21 = matrix.At(2, 1);
        const double m22 = matrix.At(2, 2);
        const double trace = m00 + m11 + m22;
        double qx = 0.0;
        double qy = 0.0;
        double qz = 0.0;
        double qw = 1.0;
        if (trace > 0.0) {
            const double s = math::Sqrt(trace + 1.0) * 2.0;
            qw = 0.25 * s;
            qx = (m21 - m12) / s;
            qy = (m02 - m20) / s;
            qz = (m10 - m01) / s;
        } else if (m00 > m11 && m00 > m22) {
            const double s = math::Sqrt(1.0 + m00 - m11 - m22) * 2.0;
            qw = (m21 - m12) / s;
            qx = 0.25 * s;
            qy = (m01 + m10) / s;
            qz = (m02 + m20) / s;
        } else if (m11 > m22) {
            const double s = math::Sqrt(1.0 + m11 - m00 - m22) * 2.0;
            qw = (m02 - m20) / s;
            qx = (m01 + m10) / s;
            qy = 0.25 * s;
            qz = (m12 + m21) / s;
        } else {
            const double s = math::Sqrt(1.0 + m22 - m00 - m11) * 2.0;
            qw = (m10 - m01) / s;
            qx = (m02 + m20) / s;
            qy = (m12 + m21) / s;
            qz = 0.25 * s;
        }
        if (qw < 0.0) {
            qx = -qx;
            qy = -qy;
            qz = -qz;
            qw = -qw;
        }
        const Quaternion<double> raw(qx, qy, qz, qw);
        auto normalized = raw.TryNormalize();
        if (!normalized) {
            return Result<Quaternion>::Failure(ErrorCode::InvalidArgument);
        }
        return Result<Quaternion>::Success(Quaternion(normalized.Value()));
    }

    friend constexpr bool operator==(const Quaternion&, const Quaternion&) noexcept = default;

    constexpr bool IsFinite() const noexcept {
        return math::IsFinite(x) && math::IsFinite(y) && math::IsFinite(z) && math::IsFinite(w);
    }

    constexpr bool Equals(const Quaternion& other, T tolerance) const noexcept {
        return math::Abs(x - other.x) <= tolerance && math::Abs(y - other.y) <= tolerance &&
               math::Abs(z - other.z) <= tolerance && math::Abs(w - other.w) <= tolerance;
    }

    constexpr T LengthSquared() const noexcept {
        return Dot(*this, *this);
    }

    T Length() const noexcept {
        return math::Sqrt(LengthSquared());
    }

    bool IsNormalized(T tolerance) const noexcept {
        return math::Abs(Length() - static_cast<T>(1)) <= tolerance;
    }

    // 실패 계약은 Vector3::TryNormalize와 같다(유한하지 않음·영 quaternion은 InvalidArgument).
    Result<Quaternion> TryNormalize() const noexcept {
        if (!IsFinite()) {
            return Result<Quaternion>::Failure(ErrorCode::InvalidArgument);
        }
        const T largest =
            math::Max(math::Max(math::Abs(x), math::Abs(y)), math::Max(math::Abs(z), math::Abs(w)));
        if (largest == static_cast<T>(0)) {
            return Result<Quaternion>::Failure(ErrorCode::InvalidArgument);
        }
        const Quaternion scaled(x / largest, y / largest, z / largest, w / largest);
        const T length = scaled.Length();
        return Result<Quaternion>::Success(
            Quaternion(scaled.x / length, scaled.y / length, scaled.z / length, scaled.w / length));
    }

    // 켤레(-x, -y, -z, w)다. 단위 quaternion에서는 역회전이다.
    constexpr Quaternion Conjugate() const noexcept {
        return Quaternion(-x, -y, -z, w);
    }

    // 길이와 무관한 역원 conj(q) / |q|²이다. 가장 큰 성분 m으로 먼저 나눠 |q/m|² ∈ [1, 4]에서
    // 계산하므로 제곱 길이가 overflow/underflow하는 입력도 처리한다.
    // 실패(InvalidArgument): 유한하지 않음·영 quaternion·역원이 표현 범위를 넘음(예: 1e-40f 크기).
    Result<Quaternion> TryInverse() const noexcept {
        if (!IsFinite()) {
            return Result<Quaternion>::Failure(ErrorCode::InvalidArgument);
        }
        const T largest =
            math::Max(math::Max(math::Abs(x), math::Abs(y)), math::Max(math::Abs(z), math::Abs(w)));
        if (largest == static_cast<T>(0)) {
            return Result<Quaternion>::Failure(ErrorCode::InvalidArgument);
        }
        const Quaternion scaled(x / largest, y / largest, z / largest, w / largest);
        const T scaledLengthSquared = scaled.LengthSquared();
        const Quaternion inverse(
            -scaled.x / scaledLengthSquared / largest, -scaled.y / scaledLengthSquared / largest,
            -scaled.z / scaledLengthSquared / largest, scaled.w / scaledLengthSquared / largest);
        if (!inverse.IsFinite()) {
            return Result<Quaternion>::Failure(ErrorCode::InvalidArgument);
        }
        return Result<Quaternion>::Success(inverse);
    }

    // Hamilton 곱 this * other. other를 먼저 적용한다. float은 platform::simd 경로를 사용한다.
    Quaternion operator*(const Quaternion& other) const noexcept {
        if constexpr (IsSameV<T, float>) {
            const auto product = platform::simd::QuaternionMultiply(
                platform::simd::Set(x, y, z, w),
                platform::simd::Set(other.x, other.y, other.z, other.w));
            return Quaternion(platform::simd::Get<0>(product), platform::simd::Get<1>(product),
                              platform::simd::Get<2>(product), platform::simd::Get<3>(product));
        } else {
            // VectorRegisterScalar::QuaternionMultiply와 같은 누적 순서다.
            const T bx = other.x;
            const T by = other.y;
            const T bz = other.z;
            const T bw = other.w;
            const T resultX = Accumulate(w * bx, x * bw, y * bz, z * -by);
            const T resultY = Accumulate(w * by, x * -bz, y * bw, z * bx);
            const T resultZ = Accumulate(w * bz, x * by, y * -bx, z * bw);
            const T resultW = Accumulate(w * bw, x * -bx, y * -by, z * -bz);
            return Quaternion(resultX, resultY, resultZ, resultW);
        }
    }

    // 단위 quaternion으로 벡터를 회전한다. t = 2(q.xyz × v), v' = v + w t + q.xyz × t.
    Vector3<T> RotateVector(const Vector3<T>& vector) const noexcept {
        const Vector3<T> axis(x, y, z);
        const Vector3<T> twice = Cross(axis, vector) * static_cast<T>(2);
        return vector + twice * w + Cross(axis, twice);
    }

    // 역회전이다. 단위 quaternion 전제다.
    Vector3<T> UnrotateVector(const Vector3<T>& vector) const noexcept {
        return Conjugate().RotateVector(vector);
    }

    // 회전된 Rocks 기준 축이다.
    Vector3<T> GetForward() const noexcept {
        return RotateVector(Vector3<T>::Forward());
    }

    Vector3<T> GetLeft() const noexcept {
        return RotateVector(Vector3<T>::Left());
    }

    Vector3<T> GetUp() const noexcept {
        return RotateVector(Vector3<T>::Up());
    }

    // 회전 행렬(열 우선, 이동 없음)이다. 단위 quaternion 전제다.
    Matrix4<T> ToMatrix() const noexcept {
        const T xx = x * x;
        const T yy = y * y;
        const T zz = z * z;
        const T xy = x * y;
        const T xz = x * z;
        const T yz = y * z;
        const T wx = w * x;
        const T wy = w * y;
        const T wz = w * z;
        const T one = static_cast<T>(1);
        const T two = static_cast<T>(2);
        Matrix4<T> result;
        result.At(0, 0) = one - two * (yy + zz);
        result.At(0, 1) = two * (xy - wz);
        result.At(0, 2) = two * (xz + wy);
        result.At(1, 0) = two * (xy + wz);
        result.At(1, 1) = one - two * (xx + zz);
        result.At(1, 2) = two * (yz - wx);
        result.At(2, 0) = two * (xz - wy);
        result.At(2, 1) = two * (yz + wx);
        result.At(2, 2) = one - two * (xx + yy);
        return result;
    }

    // 단위 회전축과 [0, pi] 각도로 분해한다. 회전이 거의 없으면 축은 +X, 각도는 0이다.
    // 단위 quaternion 전제이며 q와 -q는 같은 결과를 낸다.
    void ToAxisAngle(Vector3<T>& unitAxis, T& radians) const noexcept {
        const Quaternion positive = w < static_cast<T>(0) ? Quaternion(-x, -y, -z, -w) : *this;
        const T sine = Vector3<T>(positive.x, positive.y, positive.z).Length();
        radians = static_cast<T>(2) * math::Atan2(sine, positive.w);
        if (sine > static_cast<T>(0)) {
            unitAxis = Vector3<T>(positive.x / sine, positive.y / sine, positive.z / sine);
        } else {
            unitAxis = Vector3<T>::UnitX();
        }
    }

private:
    static T Accumulate(T p0, T p1, T p2, T p3) noexcept {
        const T s01 = p0 + p1;
        const T s012 = s01 + p2;
        return s012 + p3;
    }
};

using Quat = Quaternion<float>;
using Quatd = Quaternion<double>;

static_assert(sizeof(Quat) == 16 && sizeof(Quatd) == 32);

template <typename T>
constexpr T Dot(const Quaternion<T>& a, const Quaternion<T>& b) noexcept {
    const T xx = a.x * b.x;
    const T yy = a.y * b.y;
    const T zz = a.z * b.z;
    const T ww = a.w * b.w;
    const T low = xx + yy;
    const T high = zz + ww;
    return low + high;
}

// 두 단위 quaternion이 나타내는 회전 사이의 최소 각(라디안, [0, pi])이다. q와 -q는 정확히 0이다.
// 2·acos(|dot|)는 |dot| ≈ 1 근처에서 조건수가 나빠(float에서 약 1e-3 rad 오차) 작은 각을 구분하지
// 못하므로, 같은 반구로 맞춘 뒤 4·atan2(|a - b|, |a + b|)로 계산한다. 단위 quaternion 전제다.
template <typename T>
T AngularDistance(const Quaternion<T>& a, const Quaternion<T>& b) noexcept {
    const T sign = Dot(a, b) < static_cast<T>(0) ? static_cast<T>(-1) : static_cast<T>(1);
    const Quaternion<T> difference(a.x - sign * b.x, a.y - sign * b.y, a.z - sign * b.z,
                                   a.w - sign * b.w);
    const Quaternion<T> sum(a.x + sign * b.x, a.y + sign * b.y, a.z + sign * b.z, a.w + sign * b.w);
    // 반올림으로 pi를 1–4 ulp 넘을 수 있어 [0, pi]로 자른다(NaN은 그대로 전파).
    return math::MinPropagateNaN(static_cast<T>(4) * math::Atan2(difference.Length(), sum.Length()),
                                 math::Pi<T>);
}

// 단위 quaternion 사이의 최단 경로 구면 보간이다. t=0이면 a, t=1이면 b와 같은 회전을 반환하며
// 결과는 정규화된다. 두 회전이 매우 가까우면 정규화 선형 보간으로 전환해 0 나눗셈을 피한다.
template <typename T>
Quaternion<T> Slerp(const Quaternion<T>& a, const Quaternion<T>& b, T t) noexcept {
    T cosine = Dot(a, b);
    Quaternion<T> target = b;
    if (cosine < static_cast<T>(0)) {
        cosine = -cosine;
        target = Quaternion<T>(-b.x, -b.y, -b.z, -b.w);
    }
    T weightA = static_cast<T>(1) - t;
    T weightB = t;
    if (cosine < static_cast<T>(0.9995)) {
        const T angle = math::Acos(cosine);
        const T sine = math::Sin(angle);
        weightA = math::Sin(weightA * angle) / sine;
        weightB = math::Sin(weightB * angle) / sine;
    }
    const Quaternion<T> blended(
        a.x * weightA + target.x * weightB, a.y * weightA + target.y * weightB,
        a.z * weightA + target.z * weightB, a.w * weightA + target.w * weightB);
    auto normalized = blended.TryNormalize();
    if (normalized) {
        return normalized.Value();
    }
    // 비유한 입력(NaN·Inf 성분, NaN t)은 결과에도 비유한 성분을 남긴다(a로 바꾸지 않는다).
    // 유한한데 정규화할 수 없는 경우(단위가 아닌 영에 가까운 입력)만 a를 반환한다.
    return blended.IsFinite() ? a : blended;
}

// 회전 행렬과 이동으로 affine 행렬 T * R을 만든다.
template <typename T>
Matrix4<T> MakeRotationTranslationMatrix(const Quaternion<T>& rotation,
                                         const Vector3<T>& translation) noexcept {
    Matrix4<T> result = rotation.ToMatrix();
    result.m[12] = translation.x;
    result.m[13] = translation.y;
    result.m[14] = translation.z;
    return result;
}

} // namespace rocks
