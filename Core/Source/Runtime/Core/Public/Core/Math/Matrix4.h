#pragma once

#include <Core/Math/Vector4.h>

namespace rocks {

namespace detail {

// 열 우선 4x4 곱 output = a * b의 기준 누적 순서다. platform::simd::MatrixMultiply와 같다.
// output은 a 또는 b와 같은 저장소여도 된다.
template <typename T>
inline void MultiplyColumnMajor(const T* a, const T* b, T* output) noexcept {
    T result[16];
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            const T p0 = a[0 * 4 + row] * b[column * 4 + 0];
            const T p1 = a[1 * 4 + row] * b[column * 4 + 1];
            const T s01 = p0 + p1;
            const T p2 = a[2 * 4 + row] * b[column * 4 + 2];
            const T s012 = s01 + p2;
            const T p3 = a[3 * 4 + row] * b[column * 4 + 3];
            result[column * 4 + row] = s012 + p3;
        }
    }
    for (int index = 0; index < 16; ++index) {
        output[index] = result[index];
    }
}

template <typename T>
inline void TransformColumnMajor(const T* matrix, const T* vector, T* output) noexcept {
    T result[4];
    for (int row = 0; row < 4; ++row) {
        const T p0 = matrix[0 * 4 + row] * vector[0];
        const T p1 = matrix[1 * 4 + row] * vector[1];
        const T s01 = p0 + p1;
        const T p2 = matrix[2 * 4 + row] * vector[2];
        const T s012 = s01 + p2;
        const T p3 = matrix[3 * 4 + row] * vector[3];
        result[row] = s012 + p3;
    }
    for (int index = 0; index < 4; ++index) {
        output[index] = result[index];
    }
}

} // namespace detail

// 4x4 행렬이다. 규약은 column vector이며 p' = M * p, 합성은 world = parent * local이다.
// 즉 A * B는 B를 먼저, A를 나중에 적용한다(UE FMatrix의 row vector 곱 순서와 반대).
// 저장은 열 우선(m[column * 4 + row])이고 이동은 마지막 열(m[12..14])에 있다. 이 메모리 배치를
// GPU 상수 버퍼·파일 형식으로 그대로 가정하지 않는다. 업로드 경계에서 배치를 명시적으로 변환한다.
// 기본 생성값은 단위 행렬이다.
// float 행렬 곱과 벡터 변환은 platform::simd(SSE2/NEON) 경로를 사용하며, 같은 연산 순서의
// scalar 기준 구현과 결과를 비교 검증한다.
template <typename T>
struct Matrix4 {
    static_assert(math::IsFloatingPointV<T>, "Matrix4 supports float and double");

    T m[16] = {static_cast<T>(1), 0, 0, 0, 0, static_cast<T>(1), 0, 0, 0, 0,
               static_cast<T>(1), 0, 0, 0, 0, static_cast<T>(1)};

    static constexpr Matrix4 Identity() noexcept {
        return Matrix4();
    }

    static constexpr Matrix4 Zero() noexcept {
        Matrix4 result;
        for (int index = 0; index < 16; ++index) {
            result.m[index] = static_cast<T>(0);
        }
        return result;
    }

    static constexpr Matrix4 FromColumns(const Vector4<T>& c0, const Vector4<T>& c1,
                                         const Vector4<T>& c2, const Vector4<T>& c3) noexcept {
        Matrix4 result;
        result.SetColumn(0, c0);
        result.SetColumn(1, c1);
        result.SetColumn(2, c2);
        result.SetColumn(3, c3);
        return result;
    }

    // 수학 표기 그대로 행 단위로 채운다. 저장은 여전히 열 우선이다.
    static constexpr Matrix4 FromRows(const Vector4<T>& r0, const Vector4<T>& r1,
                                      const Vector4<T>& r2, const Vector4<T>& r3) noexcept {
        return FromColumns(r0, r1, r2, r3).Transposed();
    }

    static constexpr Matrix4 Translation(const Vector3<T>& offset) noexcept {
        Matrix4 result;
        result.m[12] = offset.x;
        result.m[13] = offset.y;
        result.m[14] = offset.z;
        return result;
    }

    static constexpr Matrix4 Scale(const Vector3<T>& scale) noexcept {
        Matrix4 result;
        result.m[0] = scale.x;
        result.m[5] = scale.y;
        result.m[10] = scale.z;
        return result;
    }

    // row, column은 0..3이다. 범위 밖은 Fatal이다.
    constexpr T& At(int row, int column) noexcept {
        CheckIndex(row, column);
        return m[column * 4 + row];
    }

    constexpr T At(int row, int column) const noexcept {
        CheckIndex(row, column);
        return m[column * 4 + row];
    }

    constexpr Vector4<T> Column(int column) const noexcept {
        CheckIndex(0, column);
        return Vector4<T>(m[column * 4], m[column * 4 + 1], m[column * 4 + 2], m[column * 4 + 3]);
    }

    constexpr Vector4<T> Row(int row) const noexcept {
        CheckIndex(row, 0);
        return Vector4<T>(m[row], m[4 + row], m[8 + row], m[12 + row]);
    }

    constexpr void SetColumn(int column, const Vector4<T>& value) noexcept {
        CheckIndex(0, column);
        m[column * 4 + 0] = value.x;
        m[column * 4 + 1] = value.y;
        m[column * 4 + 2] = value.z;
        m[column * 4 + 3] = value.w;
    }

    // 마지막 열의 xyz다. affine 행렬의 이동 성분이다.
    constexpr Vector3<T> GetTranslation() const noexcept {
        return Vector3<T>(m[12], m[13], m[14]);
    }

    friend constexpr bool operator==(const Matrix4&, const Matrix4&) noexcept = default;

    constexpr bool IsFinite() const noexcept {
        for (int index = 0; index < 16; ++index) {
            if (!math::IsFinite(m[index])) {
                return false;
            }
        }
        return true;
    }

    // 모든 원소의 |차이| <= tolerance.
    constexpr bool Equals(const Matrix4& other, T tolerance) const noexcept {
        for (int index = 0; index < 16; ++index) {
            if (!(math::Abs(m[index] - other.m[index]) <= tolerance)) {
                return false;
            }
        }
        return true;
    }

    constexpr Matrix4 Transposed() const noexcept {
        Matrix4 result;
        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                result.m[row * 4 + column] = m[column * 4 + row];
            }
        }
        return result;
    }

    // this * other. other를 먼저 적용하는 합성이다.
    Matrix4 operator*(const Matrix4& other) const noexcept {
        Matrix4 result;
        if constexpr (IsSameV<T, float>) {
            platform::simd::MatrixMultiply(m, other.m, result.m);
        } else {
            detail::MultiplyColumnMajor(m, other.m, result.m);
        }
        return result;
    }

    // 동차 좌표 변환 M * v. 투영 행렬의 원근 나눗셈은 호출자가 수행한다.
    Vector4<T> operator*(const Vector4<T>& vector) const noexcept {
        if constexpr (IsSameV<T, float>) {
            const auto transformed = platform::simd::TransformVector4(
                m, platform::simd::Set(vector.x, vector.y, vector.z, vector.w));
            return Vector4<T>(
                platform::simd::Get<0>(transformed), platform::simd::Get<1>(transformed),
                platform::simd::Get<2>(transformed), platform::simd::Get<3>(transformed));
        } else {
            const T input[4] = {vector.x, vector.y, vector.z, vector.w};
            T output[4];
            detail::TransformColumnMajor(m, input, output);
            return Vector4<T>(output[0], output[1], output[2], output[3]);
        }
    }

    // affine 행렬 전제의 점 변환이다(w=1, 마지막 행은 무시). 투영 행렬에는 operator*(Vector4)를
    // 쓴다.
    Vector3<T> TransformPoint(const Vector3<T>& point) const noexcept {
        return (*this * Vector4<T>::Point(point)).Xyz();
    }

    // 방향·변위 변환이다(w=0, 이동 무시). 법선은 TryInverse 후 전치 행렬로 변환한다.
    Vector3<T> TransformVector(const Vector3<T>& vector) const noexcept {
        return (*this * Vector4<T>::Direction(vector)).Xyz();
    }

    // 행렬식이다. float 행렬도 double로 계산한 뒤 반올림한다.
    T Determinant() const noexcept {
        double source[16];
        for (int index = 0; index < 16; ++index) {
            source[index] = static_cast<double>(m[index]);
        }
        return static_cast<T>(DeterminantOf(source));
    }

    // 역행렬이다. Laplace 전개(2x2 소행렬식)로 double에서 계산한다.
    // 실패(InvalidArgument): 원소 또는 행렬식이 유한하지 않음, 행렬식 0, 결과가 T로 유한하지
    // 않음. 행렬식이 0이 아닌 근특이 행렬은 큰 값을 그대로 반환하므로 조건수 판단이 필요한
    // 호출자는 결과를 별도로 검증한다. 실패해도 이 행렬은 바뀌지 않는다.
    Result<Matrix4> TryInverse() const noexcept {
        if (!IsFinite()) {
            return Result<Matrix4>::Failure(ErrorCode::InvalidArgument);
        }
        double a[16];
        for (int index = 0; index < 16; ++index) {
            a[index] = static_cast<double>(m[index]);
        }
        double inverse[16];
        if (!InvertDouble(a, inverse)) {
            return Result<Matrix4>::Failure(ErrorCode::InvalidArgument);
        }
        Matrix4 result;
        for (int index = 0; index < 16; ++index) {
            result.m[index] = static_cast<T>(inverse[index]);
        }
        if (!result.IsFinite()) {
            return Result<Matrix4>::Failure(ErrorCode::InvalidArgument);
        }
        return Result<Matrix4>::Success(result);
    }

private:
    static constexpr void CheckIndex(int row, int column) noexcept {
        if (row < 0 || row > 3 || column < 0 || column > 3) {
            platform::Fatal("Matrix4 index out of range");
        }
    }

    // a(r, c) = a[c * 4 + r]
    static double DeterminantOf(const double* a) noexcept {
        double s[6];
        double c[6];
        TwoByTwo(a, s, c);
        return s[0] * c[5] - s[1] * c[4] + s[2] * c[3] + s[3] * c[2] - s[4] * c[1] + s[5] * c[0];
    }

    // 위 두 행과 아래 두 행의 2x2 소행렬식이다(Laplace 전개).
    static void TwoByTwo(const double* a, double* s, double* c) noexcept {
        auto at = [a](int row, int column) { return a[column * 4 + row]; };
        s[0] = at(0, 0) * at(1, 1) - at(1, 0) * at(0, 1);
        s[1] = at(0, 0) * at(1, 2) - at(1, 0) * at(0, 2);
        s[2] = at(0, 0) * at(1, 3) - at(1, 0) * at(0, 3);
        s[3] = at(0, 1) * at(1, 2) - at(1, 1) * at(0, 2);
        s[4] = at(0, 1) * at(1, 3) - at(1, 1) * at(0, 3);
        s[5] = at(0, 2) * at(1, 3) - at(1, 2) * at(0, 3);
        c[5] = at(2, 2) * at(3, 3) - at(3, 2) * at(2, 3);
        c[4] = at(2, 1) * at(3, 3) - at(3, 1) * at(2, 3);
        c[3] = at(2, 1) * at(3, 2) - at(3, 1) * at(2, 2);
        c[2] = at(2, 0) * at(3, 3) - at(3, 0) * at(2, 3);
        c[1] = at(2, 0) * at(3, 2) - at(3, 0) * at(2, 2);
        c[0] = at(2, 0) * at(3, 1) - at(3, 0) * at(2, 1);
    }

    static bool InvertDouble(const double* a, double* output) noexcept {
        double s[6];
        double c[6];
        TwoByTwo(a, s, c);
        const double determinant =
            s[0] * c[5] - s[1] * c[4] + s[2] * c[3] + s[3] * c[2] - s[4] * c[1] + s[5] * c[0];
        if (determinant == 0.0 || !math::IsFinite(determinant)) {
            return false;
        }
        auto at = [a](int row, int column) { return a[column * 4 + row]; };
        double b[16];
        auto set = [&b](int row, int column, double value) { b[column * 4 + row] = value; };
        set(0, 0, at(1, 1) * c[5] - at(1, 2) * c[4] + at(1, 3) * c[3]);
        set(0, 1, -at(0, 1) * c[5] + at(0, 2) * c[4] - at(0, 3) * c[3]);
        set(0, 2, at(3, 1) * s[5] - at(3, 2) * s[4] + at(3, 3) * s[3]);
        set(0, 3, -at(2, 1) * s[5] + at(2, 2) * s[4] - at(2, 3) * s[3]);
        set(1, 0, -at(1, 0) * c[5] + at(1, 2) * c[2] - at(1, 3) * c[1]);
        set(1, 1, at(0, 0) * c[5] - at(0, 2) * c[2] + at(0, 3) * c[1]);
        set(1, 2, -at(3, 0) * s[5] + at(3, 2) * s[2] - at(3, 3) * s[1]);
        set(1, 3, at(2, 0) * s[5] - at(2, 2) * s[2] + at(2, 3) * s[1]);
        set(2, 0, at(1, 0) * c[4] - at(1, 1) * c[2] + at(1, 3) * c[0]);
        set(2, 1, -at(0, 0) * c[4] + at(0, 1) * c[2] - at(0, 3) * c[0]);
        set(2, 2, at(3, 0) * s[4] - at(3, 1) * s[2] + at(3, 3) * s[0]);
        set(2, 3, -at(2, 0) * s[4] + at(2, 1) * s[2] - at(2, 3) * s[0]);
        set(3, 0, -at(1, 0) * c[3] + at(1, 1) * c[1] - at(1, 2) * c[0]);
        set(3, 1, at(0, 0) * c[3] - at(0, 1) * c[1] + at(0, 2) * c[0]);
        set(3, 2, -at(3, 0) * s[3] + at(3, 1) * s[1] - at(3, 2) * s[0]);
        set(3, 3, at(2, 0) * s[3] - at(2, 1) * s[1] + at(2, 2) * s[0]);
        const double inverseDeterminant = 1.0 / determinant;
        for (int index = 0; index < 16; ++index) {
            output[index] = b[index] * inverseDeterminant;
            if (!math::IsFinite(output[index])) {
                return false;
            }
        }
        return true;
    }
};

using Mat4 = Matrix4<float>;
using Mat4d = Matrix4<double>;

static_assert(sizeof(Mat4) == 64 && sizeof(Mat4d) == 128);

} // namespace rocks
