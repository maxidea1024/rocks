#pragma once

#include <Core/Math/Quaternion.h>

// Core의 TRS 변환 세 가지다. 셋 다 이동·회전(단위 quaternion)·축별 scale로 표현하고 점 변환 순서
// (scale → rotation → translation: p' = T * R * S * p), 합성 규약(world = parent * local), 실패
// 규약(shear는 근사하지 않고 Unsupported, 0 scale 역변환은 InvalidArgument)이 같다
// (Docs/Design/EngineImplementationPlan.ko.md 2.3절, ReflectionExtensions.ko.md 3.2절).
// - Transform: authoring LocalTransform이다. 이동 double(Vec3d), 회전·scale float(Quat·Vec3)이고
//   계산은 double이다. 큰 월드 좌표를 다루는 편집·씬 계층의 기본 변환이다.
// - Transform3f(= Transform3<float>): 모든 성분이 float이고 점·벡터 변환과 행렬 곱도 float로
//   계산한다(분해와 역 TRS 구성만 double로 한 뒤 반올림 — Transform3 주석). UE FTransform3f
//   데이터 반입, GPU·애니메이션처럼 float로 충분한 경로용이다.
// - Transform3d(= Transform3<double>): 모든 성분과 계산이 double이다. UE FTransform3d(=
//   FTransform) 데이터 반입, 도구·오프라인 계산용이다.
//
// 셋 사이에는 암시 변환·변환 생성자가 없고 이름 있는 함수만 있다. To…는 정확한 넓힘이고,
// RoundTo…는 정밀도가 줄어드는 성분을 성분마다 가장 가까운 값(같은 거리면 짝수 쪽 — IEEE 기본
// 반올림)으로 바꾼다. 회전은 반올림만 하고 다시 정규화하지 않는다(단위 회전이면 길이 오차가
// float 반올림 수준이라 IsValid 기본 허용치 안이다). 좁힌 결과가 float 범위를 넘으면 ±Inf, 아주
// 작으면 0·subnormal이 되므로 외부 값을 좁힌 호출자는 IsValid와 필요한 역변환의 성공 여부로
// 확인한다. 범위 밖 좁힘이 ±Inf인 것은 Clang의 정의(float 표현 범위를 [-inf, +inf]로 본다)이고,
// 반올림·subnormal 결과는 IEEE-754 기본 부동소수점 환경(최근접 짝수 반올림, FTZ/DAZ 꺼짐 —
// Platform/FloatingPointEnvironment.h)을 전제한다. fast-math 빌드는 Base/NumericLimits.h가 막는다.
//   Transform   → Transform3d: Transform::ToTransform3d()          (정확)
//   Transform   → Transform3f: Transform::RoundToTransform3f()     (이동 반올림)
//   Transform3f → Transform:   Transform3f::ToTransform()          (정확)
//   Transform3f → Transform3d: Transform3f::ToTransform3d()        (정확)
//   Transform3d → Transform:   Transform3d::RoundToTransform()     (회전·scale 반올림)
//   Transform3d → Transform3f: Transform3d::RoundToTransform3f()   (모든 성분 반올림)
//
// UE 5.8 TTransform<T>와 다른 점(FTransform·FTransform3d = TTransform<double>, FTransform3f =
// TTransform<float> — Core/Public/Math/MathFwd.h. 근거 행은 ReflectionExtensions.ko.md 3.2절):
// - 합성 순서: UE A * B는 A를 먼저 적용한다. Rocks TryCompose(parent, local)은 local을
//   먼저 적용한다.
// - UE Multiply는 음수 scale이 없으면 회전·scale·이동을 성분별로 곱해 비균일 scale과
//   회전이 만드는 shear를 조용히 버린다(Core/Private/Math/TransformVectorized.cpp). Rocks는
//   Unsupported로 실패한다.
// - UE Inverse는 세 축 scale이 모두 거의 0일 때만 항등을 돌려주고, 그 밖에는 InverseFast가
//   거의 0인 축의 역수를 0으로 두며 비균일 scale과 회전의 shear도 버린다.
//   InverseTransformPosition도 거의 0인 축의 역수를 0으로 둔다
//   (Core/Public/Math/TransformVectorized.h). Rocks는 InvalidArgument·Unsupported로 실패한다.
// - UE float↔double 변환은 explicit 생성자다(TransformVectorized.h). Rocks는 좁힘을 함수
//   이름(RoundTo…)으로 드러낸다.
// - UE 멤버 선언 순서는 Rotation·Translation·Scale3D다(TransformVectorized.h,
//   CoreUObject/Public/UObject/NoExportTypes.h). Rocks는 셋 다 translation·rotation·scale이다.
//   UE FTransform3f·3d는 immutable 이진(태그 없이 UE 선언 순서)으로 저장되므로 반입은 그
//   순서로 읽어 멤버 이름으로 대응시킨다(Scale3D → scale).
// - UE FTransform은 회전·scale도 double이다. Rocks Transform은 회전·scale이 float다(EIP
//   2.2절 "큰 월드"). UE FTransform 값은 Transform3d로 받은 뒤 RoundToTransform으로 옮긴다.
namespace rocks {

namespace detail {

// TryDecomposeAffine의 결과다. scale은 반사 부호를 포함한 열 길이, rotation은 w >= 0 단위
// quaternion이다.
struct AffineTrsParts {
    Vec3d translation;
    Quatd rotation;
    Vec3d scale;
};

// TryDecomposeAffine의 열 길이 계산 방식이다.
// - Direct: sqrt(x² + y² + z²) 그대로다. 최대 성분이 약 1e±154 밖이면 제곱이 overflow·
//   underflow해 0 길이·비유한(InvalidArgument)이나 정밀도 손실이 생긴다. 혼합 정밀도 Transform이
//   쓰며 지금까지의 결과(극단 열에서 shear 판정보다 InvalidArgument가 먼저 나는 순서 포함)를
//   그대로 둔다.
// - Rescaled: 최대 성분이 [AffineColumnDirectMin, AffineColumnDirectMax] 밖일 때만 그 성분으로
//   나눠 계산한다. 범위 안에서는 Direct와 비트가 같다. Transform3f·Transform3d가 쓴다.
enum class AffineColumnLength : uint8 {
    Direct,
    Rescaled,
};

// Rescaled가 Direct와 같은 식으로 계산하는 최대 성분 크기 범위다. 제곱이 [2^-1000, 2^1000]
// 안이라 overflow도 subnormal도 생기지 않는다.
inline constexpr double AffineColumnDirectMin = 0x1p-500;
inline constexpr double AffineColumnDirectMax = 0x1p500;

inline double ColumnLength(const Vec3d& column, AffineColumnLength mode) noexcept {
    if (mode == AffineColumnLength::Rescaled) {
        const double largest = column.MaxAbsComponent();
        if (largest > AffineColumnDirectMax || (largest > 0.0 && largest < AffineColumnDirectMin)) {
            return largest * (column / largest).Length();
        }
    }

    return column.Length();
}

// 세 Transform의 TryFromMatrix가 같이 쓰는 affine → TRS 분해다(double 계산). 열 길이를 scale로,
// 정규화한 열을 회전으로 사용하고 행렬식이 음수면 x scale의 부호를 뒤집는다. 회전 열의 직교성
// 오차가 orthogonalityTolerance를 넘으면 shear로 판단해 Unsupported, 비유한 원소·마지막 행이
// (0, 0, 0, 1)이 아님·0 길이(또는 비유한 길이) 열이면 InvalidArgument다. 열 길이 방식은
// AffineColumnLength를 본다.
inline Result<AffineTrsParts> TryDecomposeAffine(const Mat4d& matrix, double orthogonalityTolerance,
                                                 AffineColumnLength lengthMode) noexcept {
    if (!matrix.IsFinite() || matrix.At(3, 0) != 0.0 || matrix.At(3, 1) != 0.0 ||
        matrix.At(3, 2) != 0.0 || matrix.At(3, 3) != 1.0) {
        return Result<AffineTrsParts>::Failure(ErrorCode::InvalidArgument);
    }

    Vec3d columns[3] = {matrix.Column(0).Xyz(), matrix.Column(1).Xyz(), matrix.Column(2).Xyz()};
    double lengths[3];
    for (int axis = 0; axis < 3; ++axis) {
        lengths[axis] = ColumnLength(columns[axis], lengthMode);
        if (!(lengths[axis] > 0.0) || !math::IsFinite(lengths[axis])) {
            return Result<AffineTrsParts>::Failure(ErrorCode::InvalidArgument);
        }

        columns[axis] = columns[axis] / lengths[axis];
    }

    if (Dot(Cross(columns[0], columns[1]), columns[2]) < 0.0) {
        lengths[0] = -lengths[0];
        columns[0] = -columns[0];
    }

    if (math::Abs(Dot(columns[0], columns[1])) > orthogonalityTolerance ||
        math::Abs(Dot(columns[0], columns[2])) > orthogonalityTolerance ||
        math::Abs(Dot(columns[1], columns[2])) > orthogonalityTolerance) {
        return Result<AffineTrsParts>::Failure(ErrorCode::Unsupported);
    }

    const Mat4d rotationMatrix =
        Mat4d::FromColumns(Vec4d(columns[0], 0.0), Vec4d(columns[1], 0.0), Vec4d(columns[2], 0.0),
                           Vec4d(0.0, 0.0, 0.0, 1.0));
    auto rotation = Quatd::TryFromRotationMatrix(rotationMatrix);
    if (!rotation) {
        return Result<AffineTrsParts>::Failure(rotation.Error());
    }

    return Result<AffineTrsParts>::Success(AffineTrsParts{
        matrix.GetTranslation(), rotation.Value(), Vec3d(lengths[0], lengths[1], lengths[2])});
}

// 역변환에 쓸 수 있는 scale인지 본다: 모든 성분이 유한하고 0이 아니며 역수도 T로 유한하다. 역수가
// 유한하지 않은 것은 |s| < 1 / NumericLimits<T>::Max()인 값뿐이다(float 약 2.94e-39, double 약
// 5.56e-309 — subnormal의 일부). 그보다 큰 subnormal scale은 받아들인다.
template <typename T>
bool HasInvertibleScale(const Vector3<T>& scale) noexcept {
    const T zero = static_cast<T>(0);
    return scale.IsFinite() && scale.x != zero && scale.y != zero && scale.z != zero &&
           (Vector3<T>::One() / scale).IsFinite();
}

// 세 축 scale의 절댓값이 tolerance 안에서 같은지 본다(부호는 무관).
template <typename T>
bool HasUniformScaleMagnitude(const Vector3<T>& scale, T tolerance) noexcept {
    const T ax = math::Abs(scale.x);
    return math::Abs(math::Abs(scale.y) - ax) <= tolerance &&
           math::Abs(math::Abs(scale.z) - ax) <= tolerance;
}

} // namespace detail

struct Transform;

// 정밀도가 하나인 TRS 변환이다(UE TTransform<T> 대응). T는 float 또는 double이며 별칭 Transform3f·
// Transform3d로 쓴다. 성분·규약·변환 함수는 파일 머리 주석을 본다. 기본값은 항등 변환이다.
//
// 정밀도: 점·벡터·방향 변환, 역변환 점·벡터, ToMatrix, TryCompose의 행렬 곱은 T로
// 계산한다(float 행렬 곱은 platform::simd 경로). TryFromMatrix의 분해는 입력을 double로
// 정확히 넓혀 계산한 뒤 회전·scale을 T로 가장 가깝게 반올림한다(float 행렬을 double로
// 계산하는 Matrix4::TryInverse·Quaternion::TryFromRotationMatrix와 같은 방식). TryInverse도 역
// affine [S⁻¹Rᵀ | -S⁻¹Rᵀt]를 double로 직접 만들어 같은 분해를 거친다(행렬식을 쓰지 않으므로
// scale 범위가 행렬식의 표현 범위에 묶이지 않는다). 그래서 같은 행렬 값의 shear·0 scale 판정은
// 세 Transform이 같고 결과의 표현 범위만 다르다(혼합 Transform은 열 성분이 약 1e±150 밖이면
// 열 길이가 넘쳐 InvalidArgument가 먼저 난다). 분해의 열 길이는 극단 크기에서도 overflow·
// underflow하지 않는다(detail::AffineColumnLength::Rescaled).
//
// rotation은 단위 quaternion이어야 한다. 길이 오차가 IsValid 허용치 안이면 모든 연산이
// 정규화한 회전을 쓴다(UnitRotation — 호출마다 정규화하므로 비용이 든다). 같은 변환을 많은 점에
// 적용할 때는 ToMatrix()를 한 번 만든 뒤 Matrix4::TransformPoint·math::TransformPoints를 쓴다.
// 음수 scale은 허용하며 행렬식 부호로 반사를 표현한다. 값 타입이며 소유하는 자원이 없다.
//
// 사용 예:
//   const Transform3f parent(Vec3(0.0f, 0.0f, 1.0f), Quat::Identity(), Vec3::Splat(2.0f));
//   const Transform3f local(Vec3(1.0f, 0.0f, 0.0f), Quat::Identity(), Vec3::One());
//   auto world = Transform3f::TryCompose(parent, local); // world = parent * local
//   if (world) {
//       const Vec3 origin = world.Value().TransformPoint(Vec3::Zero()); // (2, 0, 1)
//       const Transform3d tool = world.Value().ToTransform3d();         // 정확한 넓힘
//   }
template <typename T>
struct Transform3 {
    static_assert(math::IsFloatingPointV<T>, "Transform3 supports float and double");

    Vector3<T> translation;
    Quaternion<T> rotation;
    Vector3<T> scale = Vector3<T>::One();

    constexpr Transform3() noexcept = default;

    constexpr Transform3(const Vector3<T>& translationValue, const Quaternion<T>& rotationValue,
                         const Vector3<T>& scaleValue) noexcept
        : translation(translationValue), rotation(rotationValue), scale(scaleValue) {
    }

    static constexpr Transform3 Identity() noexcept {
        return Transform3();
    }

    // 모든 성분이 유한하고 rotation 길이 오차가 rotationTolerance 이하이면 true다. 0 scale도
    // 유효하다(역변환만 실패한다). 외부 입력과 RoundTo… 결과를 검사할 때 쓴다.
    bool IsValid(T rotationTolerance = static_cast<T>(1.0e-4)) const noexcept {
        return translation.IsFinite() && scale.IsFinite() && rotation.IsFinite() &&
               rotation.IsNormalized(rotationTolerance);
    }

    // 세 축 scale의 절댓값이 tolerance 안에서 같으면(부호는 무관) 균일 scale이다.
    bool HasUniformScale(T tolerance) const noexcept {
        return detail::HasUniformScaleMagnitude(scale, tolerance);
    }

    // 점 변환 T * R * S * p다.
    Vector3<T> TransformPoint(const Vector3<T>& point) const noexcept {
        return UnitRotation().RotateVector(point * scale) + translation;
    }

    // 방향·변위 변환이다. 이동은 적용하지 않고 scale은 적용한다.
    Vector3<T> TransformVector(const Vector3<T>& vector) const noexcept {
        return UnitRotation().RotateVector(vector * scale);
    }

    // scale 없이 회전만 적용한다(단위 방향 유지).
    Vector3<T> TransformDirection(const Vector3<T>& direction) const noexcept {
        return UnitRotation().RotateVector(direction);
    }

    // 역변환 점 변환 S⁻¹ R⁻¹ (p - t)다. 실패(InvalidArgument): scale 성분이 0·비유한이거나 역수가
    // T로 유한하지 않음(detail::HasInvertibleScale). 실패해도 이 값은 바뀌지 않는다. 역수가
    // 유한해도 결과가 T 범위를 넘으면 비유한 성분으로 성공한다(예: Transform3f scale 5e-39에서
    // x = 1000 → +Inf). 유한한 결과가 필요한 호출자는 결과의 IsFinite를 본다.
    Result<Vector3<T>> TryInverseTransformPoint(const Vector3<T>& point) const noexcept {
        if (!detail::HasInvertibleScale(scale)) {
            return Result<Vector3<T>>::Failure(ErrorCode::InvalidArgument);
        }

        return Result<Vector3<T>>::Success(UnitRotation().UnrotateVector(point - translation) /
                                           scale);
    }

    // 역변환 벡터 변환 S⁻¹ R⁻¹ v다. 실패와 비유한 결과는 TryInverseTransformPoint와 같다.
    Result<Vector3<T>> TryInverseTransformVector(const Vector3<T>& vector) const noexcept {
        if (!detail::HasInvertibleScale(scale)) {
            return Result<Vector3<T>>::Failure(ErrorCode::InvalidArgument);
        }

        return Result<Vector3<T>>::Success(UnitRotation().UnrotateVector(vector) / scale);
    }

    // affine 행렬 T * R * S(column vector)를 T 정밀도로 만든다(Transform3f는 Mat4, Transform3d는
    // Mat4d).
    Matrix4<T> ToMatrix() const noexcept {
        Matrix4<T> result = UnitRotation().ToMatrix();
        for (int row = 0; row < 3; ++row) {
            result.At(row, 0) *= scale.x;
            result.At(row, 1) *= scale.y;
            result.At(row, 2) *= scale.z;
        }

        result.m[12] = translation.x;
        result.m[13] = translation.y;
        result.m[14] = translation.z;
        return result;
    }

    // affine 행렬을 TRS로 분해한다(detail::TryDecomposeAffine — double 계산 뒤 T로 반올림). 회전
    // 열의 직교성 오차가 orthogonalityTolerance를 넘으면 shear로 판단해 Unsupported, 비유한
    // 원소·마지막 행이 (0, 0, 0, 1)이 아님·0 scale(T로 반올림한 뒤 0이나 비유한이 된 경우 포함)이면
    // InvalidArgument다. 반사(행렬식 음수)는 x scale 부호로 표현한다.
    static Result<Transform3> TryFromMatrix(const Matrix4<T>& matrix,
                                            double orthogonalityTolerance = 1.0e-5) noexcept {
        return FromParts(detail::TryDecomposeAffine(Mat4d(matrix), orthogonalityTolerance,
                                                    detail::AffineColumnLength::Rescaled));
    }

    // parent * local을 TRS로 계산한다. TryFromMatrix(parent.ToMatrix() * local.ToMatrix())와 같다.
    // shear가 생기면 Unsupported, 비유한값은 InvalidArgument다. 0 scale은 곱에 0 길이 열이 남으면
    // InvalidArgument이고, 0 scale과 회전이 평행한 열을 만들면 TRS로 표현할 수 없어 Unsupported다
    // (어느 쪽인지는 회전 행렬 원소의 반올림에 따라 달라지므로 0 scale은 HasInvertibleScale 등으로
    // 먼저 판정한다). shear를 포함한 정확한 affine 합성이 필요하면 parent.ToMatrix() *
    // local.ToMatrix()를 직접 쓴다.
    static Result<Transform3> TryCompose(const Transform3& parent, const Transform3& local,
                                         double orthogonalityTolerance = 1.0e-5) noexcept {
        return TryFromMatrix(parent.ToMatrix() * local.ToMatrix(), orthogonalityTolerance);
    }

    // 역변환을 TRS로 계산한다. 역 affine [S⁻¹Rᵀ | -S⁻¹Rᵀt]를 double로 만든 뒤 TryFromMatrix와 같은
    // 분해로 T에 반올림한다. 비균일 scale과 회전이 함께 있으면 일반적으로 표현할 수 없어
    // Unsupported다. 역변환할 수 없는 scale(detail::HasInvertibleScale 위반)과 결과가 T 범위를
    // 넘는 경우(역 이동·역 scale이 비유한)는 InvalidArgument다.
    Result<Transform3> TryInverse(double orthogonalityTolerance = 1.0e-5) const noexcept {
        if (!detail::HasInvertibleScale(scale)) {
            return Result<Transform3>::Failure(ErrorCode::InvalidArgument);
        }

        const Mat4d transposed = Quatd(UnitRotation()).ToMatrix().Transposed();
        const Vec3d axisScale(scale);
        const double axisScales[3] = {axisScale.x, axisScale.y, axisScale.z};
        Mat4d inverse;
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                inverse.At(row, column) = transposed.At(row, column) / axisScales[row];
            }
        }

        const Vec3d moved = transposed.TransformVector(Vec3d(translation)) / axisScale;
        inverse.m[12] = -moved.x;
        inverse.m[13] = -moved.y;
        inverse.m[14] = -moved.z;
        return FromParts(detail::TryDecomposeAffine(inverse, orthogonalityTolerance,
                                                    detail::AffineColumnLength::Rescaled));
    }

    // Transform3f → Transform3d 넓힘이다. 모든 성분이 정확히 보존된다.
    Transform3<double> ToTransform3d() const noexcept
        requires(IsSameV<T, float>)
    {
        return Transform3<double>(Vector3<double>(translation), Quaternion<double>(rotation),
                                  Vector3<double>(scale));
    }

    // Transform3d → Transform3f 좁힘이다. 모든 성분을 float로 가장 가깝게 반올림한다.
    Transform3<float> RoundToTransform3f() const noexcept
        requires(IsSameV<T, double>)
    {
        return Transform3<float>(Vector3<float>(translation), Quaternion<float>(rotation),
                                 Vector3<float>(scale));
    }

    // Transform3f → Transform 넓힘이다(이동 float → double). 모든 성분이 정확히 보존된다.
    Transform ToTransform() const noexcept
        requires(IsSameV<T, float>);

    // Transform3d → Transform 좁힘이다. 이동은 그대로, 회전·scale은 float로 가장 가깝게 반올림한다.
    Transform RoundToTransform() const noexcept
        requires(IsSameV<T, double>);

private:
    // UnitRotation이 sqrt 한 번으로 정규화하는 제곱 길이 범위다. 그 밖(0 근처·아주 큼·비유한)은
    // 성분을 먼저 나누는 Quaternion::TryNormalize로 넘긴다.
    static constexpr T FastNormalizeMinLengthSquared = static_cast<T>(0x1p-60);
    static constexpr T FastNormalizeMaxLengthSquared = static_cast<T>(0x1p60);

    // 길이 오차가 있는 rotation을 정규화한다. 제곱 길이가 위 범위면 1 / sqrt 한 번과 곱 네 번이고,
    // 그 밖은 TryNormalize다. 정규화할 수 없는 값(IsValid 위반)은 그대로 사용하며 결과는 의미가
    // 없다. IEEE 기본 연산과 정확히 반올림되는 sqrt만 쓰므로 결정적이다.
    Quaternion<T> UnitRotation() const noexcept {
        const T lengthSquared = rotation.LengthSquared();
        if (lengthSquared >= FastNormalizeMinLengthSquared &&
            lengthSquared <= FastNormalizeMaxLengthSquared) {
            const T inverseLength = static_cast<T>(1) / math::Sqrt(lengthSquared);
            return Quaternion<T>(rotation.x * inverseLength, rotation.y * inverseLength,
                                 rotation.z * inverseLength, rotation.w * inverseLength);
        }

        auto normalized = rotation.TryNormalize();
        return normalized ? normalized.Value() : rotation;
    }

    // double 분해 결과를 T로 반올림한다. 분해 실패는 그대로 전하고, 반올림한 scale이 0·비유한이거나
    // 이동이 비유한이면 InvalidArgument다.
    static Result<Transform3> FromParts(const Result<detail::AffineTrsParts>& parts) noexcept {
        if (!parts) {
            return Result<Transform3>::Failure(parts.Error());
        }

        const Vector3<T> roundedTranslation(parts.Value().translation);
        const Vector3<T> roundedScale(parts.Value().scale);
        const T zero = static_cast<T>(0);
        if (!roundedTranslation.IsFinite() || !roundedScale.IsFinite() || roundedScale.x == zero ||
            roundedScale.y == zero || roundedScale.z == zero) {
            return Result<Transform3>::Failure(ErrorCode::InvalidArgument);
        }

        return Result<Transform3>::Success(
            Transform3(roundedTranslation, Quaternion<T>(parts.Value().rotation), roundedScale));
    }
};

using Transform3f = Transform3<float>;
using Transform3d = Transform3<double>;

static_assert(sizeof(Transform3f) == 40 && sizeof(Transform3d) == 80);

// 이동·회전·축별 scale(TRS)로 표현한 authoring 변환이다. 설계 규약(§2.3)에 따라 이동은
// double(Vec3d), 회전은 float quaternion, scale은 float이다. 계산은 모두 double로 수행한다.
// 점 변환 순서는 scale → rotation → translation: p' = T * R * S * p.
// 합성 규약은 world = parent * local이다. UE FTransform의 A * B(A 먼저 적용)와 순서가 반대다.
//
// 비균일 parent scale과 child 회전을 합성하면 shear가 생겨 TRS로 표현할 수 없다. 이 경우
// TryCompose는 근사하지 않고 Unsupported로 실패하며, 정확한 결과가 필요하면 ToMatrix()의
// affine 곱(Mat4d)을 사용한다. 음수 scale은 허용하며 행렬식 부호로 반사를 표현한다.
// rotation은 단위 quaternion이어야 한다. 외부 입력은 IsValid로 검사한다. 기본값은 항등 변환이다.
// 정밀도가 하나인 Transform3f·Transform3d와의 관계·변환 함수는 파일 머리 주석을 본다.
struct Transform {
    Vec3d translation;
    Quat rotation;
    Vec3 scale = Vec3::One();

    constexpr Transform() noexcept = default;

    constexpr Transform(const Vec3d& translationValue, const Quat& rotationValue,
                        const Vec3& scaleValue) noexcept
        : translation(translationValue), rotation(rotationValue), scale(scaleValue) {
    }

    static constexpr Transform Identity() noexcept {
        return Transform();
    }

    // 모든 성분이 유한하고 rotation 길이 오차가 rotationTolerance 이하이면 true다.
    bool IsValid(float rotationTolerance = 1.0e-4f) const noexcept {
        return translation.IsFinite() && scale.IsFinite() && rotation.IsFinite() &&
               rotation.IsNormalized(rotationTolerance);
    }

    // 세 축 scale의 절댓값이 같으면(부호는 무관) 균일 scale이다.
    bool HasUniformScale(float tolerance) const noexcept {
        return detail::HasUniformScaleMagnitude(scale, tolerance);
    }

    Vec3d TransformPoint(const Vec3d& point) const noexcept {
        return Rotation().RotateVector(point * ScaleD()) + translation;
    }

    // 방향·변위 변환이다. 이동은 적용하지 않고 scale은 적용한다.
    Vec3d TransformVector(const Vec3d& vector) const noexcept {
        return Rotation().RotateVector(vector * ScaleD());
    }

    // scale 없이 회전만 적용한다(단위 방향 유지).
    Vec3d TransformDirection(const Vec3d& direction) const noexcept {
        return Rotation().RotateVector(direction);
    }

    // 역변환 점 변환 S⁻¹ R⁻¹ (p - t). 실패(InvalidArgument): scale 성분 중 0 또는 비유한값.
    // double로 계산하므로 결과가 double 범위를 넘을 때만 비유한 성분으로 성공한다.
    Result<Vec3d> TryInverseTransformPoint(const Vec3d& point) const noexcept {
        if (!HasInvertibleScale()) {
            return Result<Vec3d>::Failure(ErrorCode::InvalidArgument);
        }

        return Result<Vec3d>::Success(Rotation().UnrotateVector(point - translation) / ScaleD());
    }

    Result<Vec3d> TryInverseTransformVector(const Vec3d& vector) const noexcept {
        if (!HasInvertibleScale()) {
            return Result<Vec3d>::Failure(ErrorCode::InvalidArgument);
        }

        return Result<Vec3d>::Success(Rotation().UnrotateVector(vector) / ScaleD());
    }

    // affine 행렬 T * R * S(column vector)를 double로 만든다.
    Mat4d ToMatrix() const noexcept {
        Mat4d result = Rotation().ToMatrix();
        const Vec3d s = ScaleD();
        for (int row = 0; row < 3; ++row) {
            result.At(row, 0) *= s.x;
            result.At(row, 1) *= s.y;
            result.At(row, 2) *= s.z;
        }

        result.m[12] = translation.x;
        result.m[13] = translation.y;
        result.m[14] = translation.z;
        return result;
    }

    // affine 행렬을 TRS로 분해한다. 열 길이를 scale로, 정규화한 열을 회전으로 사용하고 행렬식이
    // 음수면 x scale의 부호를 뒤집는다. 회전 열의 직교성 오차가 orthogonalityTolerance를 넘으면
    // shear로 판단해 Unsupported, 비유한 원소·마지막 행이 (0,0,0,1)이 아님·0 scale이면
    // InvalidArgument다. 회전은 float으로, scale은 float으로 반올림된다.
    static Result<Transform> TryFromMatrix(const Mat4d& matrix,
                                           double orthogonalityTolerance = 1.0e-5) noexcept {
        auto parts = detail::TryDecomposeAffine(matrix, orthogonalityTolerance,
                                                detail::AffineColumnLength::Direct);
        if (!parts) {
            return Result<Transform>::Failure(parts.Error());
        }

        const Vec3 rounded(parts.Value().scale);
        if (!rounded.IsFinite() || rounded.x == 0.0f || rounded.y == 0.0f || rounded.z == 0.0f) {
            return Result<Transform>::Failure(ErrorCode::InvalidArgument);
        }

        return Result<Transform>::Success(
            Transform(parts.Value().translation, Quat(parts.Value().rotation), rounded));
    }

    // parent * local을 TRS로 계산한다. shear가 생기면 Unsupported, 비유한값·0 scale은
    // InvalidArgument다. 정확한 affine 합성은 parent.ToMatrix() * local.ToMatrix()를 쓴다.
    static Result<Transform> TryCompose(const Transform& parent, const Transform& local,
                                        double orthogonalityTolerance = 1.0e-5) noexcept {
        return TryFromMatrix(parent.ToMatrix() * local.ToMatrix(), orthogonalityTolerance);
    }

    // 역변환을 TRS로 계산한다. 비균일 scale과 회전이 함께 있으면 일반적으로 표현할 수 없어
    // Unsupported다. 0 scale은 InvalidArgument다.
    Result<Transform> TryInverse(double orthogonalityTolerance = 1.0e-5) const noexcept {
        if (!HasInvertibleScale()) {
            return Result<Transform>::Failure(ErrorCode::InvalidArgument);
        }

        auto inverse = ToMatrix().TryInverse();
        if (!inverse) {
            return Result<Transform>::Failure(inverse.Error());
        }

        // affine 행렬의 역도 affine이다. 일반 4x4 역행렬의 반올림이 마지막 행에 남긴 미세 오차는
        // 수학적으로 정확한 (0, 0, 0, 1)로 고정한다.
        Mat4d affine = inverse.Value();
        affine.At(3, 0) = 0.0;
        affine.At(3, 1) = 0.0;
        affine.At(3, 2) = 0.0;
        affine.At(3, 3) = 1.0;
        return TryFromMatrix(affine, orthogonalityTolerance);
    }

    // Transform → Transform3d 넓힘이다(회전·scale float → double). 모든 성분이 정확히 보존된다.
    Transform3d ToTransform3d() const noexcept {
        return Transform3d(translation, Quatd(rotation), Vec3d(scale));
    }

    // Transform → Transform3f 좁힘이다. 이동만 float로 가장 가깝게 반올림하고 회전·scale은
    // 그대로다. 원점에서 먼 이동은 float 간격으로 양자화된다(1e9 근처는 64 단위).
    Transform3f RoundToTransform3f() const noexcept {
        return Transform3f(Vec3(translation), rotation, scale);
    }

private:
    // float quaternion은 저장 반올림으로 길이가 1에서 약 1e-7 벗어날 수 있다. double로 확장한 뒤
    // 다시 정규화해 회전·역회전 왕복과 행렬 경로가 같은 단위 회전을 쓰게 한다. 정규화할 수 없는
    // 값(IsValid 위반)은 그대로 사용하며 결과는 의미가 없다.
    Quatd Rotation() const noexcept {
        const Quatd widened(rotation);
        auto normalized = widened.TryNormalize();
        return normalized ? normalized.Value() : widened;
    }

    Vec3d ScaleD() const noexcept {
        return Vec3d(scale);
    }

    // double로 계산하므로 float scale은 0이 아니고 유한하면 역수도 유한하다.
    bool HasInvertibleScale() const noexcept {
        return detail::HasInvertibleScale(ScaleD());
    }
};

template <typename T>
Transform Transform3<T>::ToTransform() const noexcept
    requires(IsSameV<T, float>)
{
    return Transform(Vec3d(translation), rotation, scale);
}

template <typename T>
Transform Transform3<T>::RoundToTransform() const noexcept
    requires(IsSameV<T, double>)
{
    return Transform(translation, Quat(rotation), Vec3(scale));
}

} // namespace rocks
