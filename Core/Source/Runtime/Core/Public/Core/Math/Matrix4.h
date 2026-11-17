#pragma once

#include <Core/Math/Vector4.h>

namespace rocks {

namespace detail {

// 행 우선 4x4 곱 output = a * b(행 벡터 규약 — a를 먼저, b를 나중에 적용)의 기준 누적 순서다.
// output[i][j] = ((a[i][0]·b[0][j] + a[i][1]·b[1][j]) + a[i][2]·b[2][j]) + a[i][3]·b[3][j]로 UE
// VectorMatrixMultiply(UnrealMathFPU.h:1538-1564)와 같은 순서다. platform::simd::MatrixMultiply(b,
// a)와 결과가 같다(곱의 피연산자 순서만 다르고 곱은 교환 가능하다). output은 a 또는 b와 같은
// 저장소여도 된다.
template <typename T>
inline void MultiplyRowMajor(const T* a, const T* b, T* output) noexcept {
    T result[16];
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            const T p0 = a[row * 4 + 0] * b[0 * 4 + column];
            const T p1 = a[row * 4 + 1] * b[1 * 4 + column];
            const T s01 = p0 + p1;
            const T p2 = a[row * 4 + 2] * b[2 * 4 + column];
            const T s012 = s01 + p2;
            const T p3 = a[row * 4 + 3] * b[3 * 4 + column];
            result[row * 4 + column] = s012 + p3;
        }
    }

    for (int index = 0; index < 16; ++index) {
        output[index] = result[index];
    }
}

// 행 벡터 변환 output = vector · matrix(행 우선)다. output[j] = Σ vector[k]·matrix[k][j]를 k 순서로
// 누적한다(UE VectorTransformVector, UnrealMathFPU.h:1619-1631과 같은 순서).
template <typename T>
inline void TransformRowVector(const T* matrix, const T* vector, T* output) noexcept {
    T result[4];
    for (int column = 0; column < 4; ++column) {
        const T p0 = vector[0] * matrix[0 * 4 + column];
        const T p1 = vector[1] * matrix[1 * 4 + column];
        const T s01 = p0 + p1;
        const T p2 = vector[2] * matrix[2 * 4 + column];
        const T s012 = s01 + p2;
        const T p3 = vector[3] * matrix[3 * 4 + column];
        result[column] = s012 + p3;
    }

    for (int index = 0; index < 4; ++index) {
        output[index] = result[index];
    }
}

} // namespace detail

// 4x4 행렬이다. 규약은 UE 5.8 FMatrix(TMatrix<T>)와 같다(Core/Public/Math/Matrix.h:28-32·:49).
// - 행 벡터 규약이다. 점·벡터는 행 벡터로 오른쪽 행렬을 곱한다: p' = p · M
//   (UE TransformFVector4·VectorTransformVector, UnrealMathFPU.h:1619-1631).
// - 합성 A * B는 A를 먼저, B를 나중에 적용한다: (A * B).TransformPoint(p) ==
//   B.TransformPoint(A.TransformPoint(p)). 부모·자식은 LocalToWorld = Local * ParentToWorld다.
//   Quaternion 곱(b 먼저)과 순서가 반대다.
// - 원소는 At(row, column) = M[row][column]이고 저장은 행 우선(m[row * 4 + column])이다. 회전·
//   scale 부분의 행 0·1·2가 축(X·Y·Z) 벡터이고 이동은 마지막 행(m[12..14] = M[3][0..2])에 있다.
//   메모리 배치는 UE FMatrix와 비트 단위로 같다. GPU 상수 버퍼·파일 형식으로는 이 배치를 그대로
//   가정하지 않고 업로드·저장 경계에서 명시한다.
// 기본 생성값은 단위 행렬이다. float 행렬 곱과 벡터 변환은 platform::simd(SSE2/NEON) 경로를
// 쓰며 같은 연산 순서의 scalar 기준 구현(detail::MultiplyRowMajor·TransformRowVector)과 결과를
// 비교 검증한다. double은 그 기준 구현이며 UE FPU 경로와 같은 누적 순서다.
//
// 사용 예:
//   const Mat4d local = Mat4d::Scale(Vec3d(2.0, 2.0, 2.0));
//   const Mat4d parent = Mat4d::Translation(Vec3d(10.0, 0.0, 0.0));
//   const Mat4d world = local * parent;                         // scale 먼저, 이동 나중
//   const Vec3d p = world.TransformPoint(Vec3d(1.0, 0.0, 0.0)); // (12, 0, 0)
template <typename T>
struct Matrix4 {
    static_assert(math::IsFloatingPointV<T>, "Matrix4 supports float and double");

    T m[16] = {static_cast<T>(1), 0, 0, 0, 0, static_cast<T>(1), 0, 0, 0, 0,
               static_cast<T>(1), 0, 0, 0, 0, static_cast<T>(1)};

    constexpr Matrix4() noexcept = default;

    // float↔double 변환은 정밀도 손실 가능성을 드러내도록 명시적으로만 허용한다(Vector3·
    // Quaternion과 같다). float → double은 정확하고, double → float은 원소마다 가장 가까운 값으로
    // 반올림한다(float 범위 밖은 ±Inf — 결과는 IsFinite로 확인한다).
    template <typename U>
        requires(math::IsFloatingPointV<U> && !IsSameV<U, T>)
    explicit constexpr Matrix4(const Matrix4<U>& other) noexcept {
        for (int index = 0; index < 16; ++index) {
            m[index] = static_cast<T>(other.m[index]);
        }
    }

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

    // 행 단위로 채운다(UE TMatrix(InX, InY, InZ, InW) 평면 생성자와 같다, Matrix.inl:27-34). affine
    // 행렬이면 r0·r1·r2가 축 벡터(w = 0), r3이 이동(w = 1)이다.
    static constexpr Matrix4 FromRows(const Vector4<T>& r0, const Vector4<T>& r1,
                                      const Vector4<T>& r2, const Vector4<T>& r3) noexcept {
        Matrix4 result;
        result.SetRow(0, r0);
        result.SetRow(1, r1);
        result.SetRow(2, r2);
        result.SetRow(3, r3);
        return result;
    }

    // 열 단위로 채운다. 저장은 여전히 행 우선이다.
    static constexpr Matrix4 FromColumns(const Vector4<T>& c0, const Vector4<T>& c1,
                                         const Vector4<T>& c2, const Vector4<T>& c3) noexcept {
        return FromRows(c0, c1, c2, c3).Transposed();
    }

    // 이동 행렬이다(UE FTranslationMatrix — 마지막 행이 offset).
    static constexpr Matrix4 Translation(const Vector3<T>& offset) noexcept {
        Matrix4 result;
        result.m[12] = offset.x;
        result.m[13] = offset.y;
        result.m[14] = offset.z;
        return result;
    }

    // 축별 scale 행렬이다(UE FScaleMatrix).
    static constexpr Matrix4 Scale(const Vector3<T>& scale) noexcept {
        Matrix4 result;
        result.m[0] = scale.x;
        result.m[5] = scale.y;
        result.m[10] = scale.z;
        return result;
    }

    // M[row][column]이다(UE M[RowIndex][ColumnIndex]). row, column은 0..3이며 범위 밖은 Fatal이다.
    constexpr T& At(int row, int column) noexcept {
        CheckIndex(row, column);
        return m[row * 4 + column];
    }

    constexpr T At(int row, int column) const noexcept {
        CheckIndex(row, column);
        return m[row * 4 + column];
    }

    constexpr Vector4<T> Row(int row) const noexcept {
        CheckIndex(row, 0);
        return Vector4<T>(m[row * 4], m[row * 4 + 1], m[row * 4 + 2], m[row * 4 + 3]);
    }

    constexpr Vector4<T> Column(int column) const noexcept {
        CheckIndex(0, column);
        return Vector4<T>(m[column], m[4 + column], m[8 + column], m[12 + column]);
    }

    constexpr void SetRow(int row, const Vector4<T>& value) noexcept {
        CheckIndex(row, 0);
        m[row * 4 + 0] = value.x;
        m[row * 4 + 1] = value.y;
        m[row * 4 + 2] = value.z;
        m[row * 4 + 3] = value.w;
    }

    // 축 벡터(행 0·1·2의 xyz)다. scale이 들어 있으면 그 길이를 포함한다(UE GetScaledAxis).
    // axis는 0..2이며 범위 밖은 Fatal이다.
    constexpr Vector3<T> GetScaledAxis(int axis) const noexcept {
        if (axis < 0 || axis > 2) {
            platform::Fatal("Matrix4 axis out of range");
        }

        return Row(axis).Xyz();
    }

    // 마지막 행의 xyz다. affine 행렬의 이동 성분이다(UE GetOrigin).
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

    // this * other다. this를 먼저, other를 나중에 적용하는 합성이다(UE FMatrix::operator*).
    // float의 platform::simd::MatrixMultiply(b, a)는 행 우선 a * b를 계산하므로 인자를 바꿔
    // 넘긴다(메모리 배치가 같아 SIMD 커널의 산술은 그대로다).
    Matrix4 operator*(const Matrix4& other) const noexcept {
        Matrix4 result;
        if constexpr (IsSameV<T, float>) {
            platform::simd::MatrixMultiply(other.m, m, result.m);
        } else {
            detail::MultiplyRowMajor(m, other.m, result.m);
        }
        return result;
    }

    // 동차 좌표 행 벡터 변환 vector · M이다(UE TransformFVector4). 투영 행렬의 원근 나눗셈은
    // 호출자가 수행한다.
    Vector4<T> TransformVector4(const Vector4<T>& vector) const noexcept {
        if constexpr (IsSameV<T, float>) {
            const auto transformed = platform::simd::TransformVector4(
                m, platform::simd::Set(vector.x, vector.y, vector.z, vector.w));
            return Vector4<T>(
                platform::simd::Get<0>(transformed), platform::simd::Get<1>(transformed),
                platform::simd::Get<2>(transformed), platform::simd::Get<3>(transformed));
        } else {
            const T input[4] = {vector.x, vector.y, vector.z, vector.w};
            T output[4];
            detail::TransformRowVector(m, input, output);
            return Vector4<T>(output[0], output[1], output[2], output[3]);
        }
    }

    // vector * M = TransformVector4(vector)다. 행 벡터 규약이라 행렬은 오른쪽에 둔다(M * vector는
    // 정의하지 않는다).
    friend Vector4<T> operator*(const Vector4<T>& vector, const Matrix4& matrix) noexcept {
        return matrix.TransformVector4(vector);
    }

    // affine 행렬 전제의 점 변환이다(w = 1, 마지막 열은 무시 — UE TransformPosition). 투영 행렬에는
    // TransformVector4를 쓴다.
    Vector3<T> TransformPoint(const Vector3<T>& point) const noexcept {
        return TransformVector4(Vector4<T>::Point(point)).Xyz();
    }

    // 방향·변위 변환이다(w = 0, 이동 무시 — UE TransformVector). 법선은 TryInverse 후 전치 행렬로
    // 변환한다.
    Vector3<T> TransformVector(const Vector3<T>& vector) const noexcept {
        return TransformVector4(Vector4<T>::Direction(vector)).Xyz();
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

    // 아래 소행렬식 식은 a를 열 우선(a(r, c) = a[c * 4 + r])으로 읽는다. 행 우선 행렬을 그렇게
    // 읽으면 전치 행렬이 되는데 det(Mᵀ) = det(M)이고 (Mᵀ)⁻¹ = (M⁻¹)ᵀ라, 같은 방식으로 써 넣은 결과
    // 메모리는 M⁻¹의 행 우선 배치 그대로다.
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
