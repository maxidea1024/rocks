#pragma once

#include <Core/Math/Quaternion.h>

// Core의 TRS 변환 세 가지다. UE 5.8 TTransform<T>(Core/Public/Math/TransformVectorized.h)와 규약이
// 같다.
// - 성분: 회전(단위 quaternion)·이동·축별 scale. 멤버 선언 순서와 생성자 인자 순서는 UE와 같이
//   rotation·translation·scale이다(TransformVectorized.h:75-79, 생성자 :207).
// - 점 변환 순서는 scale → rotation → translation이다: p' = rotation.RotateVector(scale · p) +
//   translation(UE TransformPosition, TransformVectorized.h:50-51·:1675-1690). 행렬로는 행 벡터
//   p · S · R · T다(ToMatrix — UE ToMatrixWithScale).
// - 합성 TryMultiply(a, b)는 UE A * B(Multiply(Out, A, B))와 같다: a를 먼저, b를 나중에 적용한다
//   (TransformVectorized.h:53-55). 부모·자식은 LocalToWorld = TryMultiply(local, parentToWorld)다
//   (UE SceneComponent.cpp:732 NewRelativeTransform * ParentToWorld). Quaternion 곱(b 먼저)과
//   순서가 반대다.
// 세 타입:
// - Transform: UE FTransform(= TTransform<double>, MathFwd.h:47) 대응이다. 모든 성분과 계산이
//   double이다. 월드·씬 계층(SceneComponent 상대 변환)과 편집 데이터의 기본 변환이며 리플렉션
//   미러는 멤버 단위(SaveGame 멤버 필터)다. 연산은 Transform3d와 같은 구현을 쓴다(값과 결과가 비트
//   단위로 같다).
// - Transform3d(= Transform3<double>): UE FTransform3d 대응이다. 값은 Transform과 같고 리플렉션
//   미러가 Atomic(통째 단위)인 점만 다르다. 도구·오프라인 계산·UE FTransform3d 데이터 반입용이다.
// - Transform3f(= Transform3<float>): UE FTransform3f 대응이다. 모든 성분이 float이고 점·벡터
//   변환과 행렬 곱도 float로 계산한다(분해와 역 TRS 구성만 double로 한 뒤 반올림). 대량 경로 —
//   애니메이션 본 pose·skinning, 렌더 카메라 상대 좌표·인스턴스 변환 — 는 Transform3f를 쓴다는
//   계약이다(UE도 본 변환은 FTransform3f, 렌더는 translated world float 행렬을 쓴다). 큰 월드 절대
//   좌표(float 간격이 1 cm를 넘는 2^24 cm ≈ 168 km 밖)는 Transform으로 두고, 카메라·타일 원점을 뺀
//   상대 값만 Transform3f로 좁힌다.
//
// 셋 사이에는 암시 변환·변환 생성자가 없고 이름 있는 함수만 있다. To…는 정확한 변환이고,
// RoundTo…는 성분마다 가장 가까운 float(같은 거리면 짝수 쪽 — IEEE 기본 반올림)로 좁힌다. 회전은
// 반올림만 하고 다시 정규화하지 않는다(단위 회전이면 길이 오차가 float 반올림 수준이라 IsValid 기본
// 허용치 안이다). 좁힌 결과가 float 범위를 넘으면 ±Inf, 아주 작으면 0·subnormal이 되므로 외부 값을
// 좁힌 호출자는 IsValid와 필요한 역변환의 성공 여부로 확인한다. 범위 밖 좁힘이 ±Inf인 것은 Clang의
// 정의(float 표현 범위를 [-inf, +inf]로 본다)이고, 반올림·subnormal 결과는 IEEE-754 기본 부동소수점
// 환경(최근접 짝수 반올림, FTZ/DAZ 꺼짐 — Platform/FloatingPointEnvironment.h)을 전제한다.
// fast-math 빌드는 Base/NumericLimits.h가 막는다.
//   Transform   → Transform3d: Transform::ToTransform3d()        (정확)
//   Transform   → Transform3f: Transform::RoundToTransform3f()   (모든 성분 반올림)
//   Transform3d → Transform:   Transform3d::ToTransform()        (정확)
//   Transform3f → Transform:   Transform3f::ToTransform()        (정확)
//   Transform3f → Transform3d: Transform3f::ToTransform3d()      (정확)
//   Transform3d → Transform3f: Transform3d::RoundToTransform3f() (모든 성분 반올림)
//
// UE 5.8 TTransform<T>와 다른 점(규약이 아니라 오류·정확도 정책 — 근거 행은
// ReflectionExtensions.ko.md 3.2절):
// - UE Multiply는 음수 scale이 없으면 회전·scale·이동을 성분별로 곱해 비균일 scale과 회전이 만드는
//   shear를 조용히 버린다(Core/Private/Math/TransformVectorized.cpp:295-345). Rocks TryMultiply는
//   행렬 곱을 분해하며 shear를 Unsupported로 실패한다. shear가 없으면 두 결과는 같은 변환이다.
// - UE Inverse는 세 축 scale이 모두 거의 0일 때만 항등을 돌려주고, 그 밖에는 InverseFast가 거의 0인
//   축의 역수를 0으로 두며 비균일 scale과 회전의 shear도 버린다. InverseTransformPosition도 거의
//   0인 축의 역수를 0으로 둔다(TransformVectorized.h:363-374·:1423). Rocks는 0 scale을
//   InvalidArgument, 표현할 수 없는 역을 Unsupported로 실패한다.
// - UE float↔double 변환은 explicit 생성자다. Rocks는 좁힘을 함수 이름(RoundTo…)으로 드러낸다.
// - UE FTransform3f·3d는 immutable 이진(태그 없이 선언 순서 Rotation·Translation·Scale3D)으로
//   저장된다. Rocks 멤버도 같은 순서이며 반입은 멤버 이름으로 대응시킨다(Scale3D → scale).
namespace rocks {

namespace detail {

// TryDecomposeAffine의 결과다. scale은 반사 부호를 포함한 축 길이, rotation은 w >= 0 단위
// quaternion이다.
struct AffineTrsParts {
    Quatd rotation;
    Vec3d translation;
    Vec3d scale;
};

// 축 길이를 그대로 sqrt(x² + y² + z²)로 계산하는 최대 성분 크기 범위다. 제곱이 [2^-1000, 2^1000]
// 안이라 overflow도 subnormal도 생기지 않는다. 범위 밖은 최대 성분으로 먼저 나눠 계산한다.
inline constexpr double AffineAxisDirectMin = 0x1p-500;
inline constexpr double AffineAxisDirectMax = 0x1p500;

inline double AxisLength(const Vec3d& axis) noexcept {
    const double largest = axis.MaxAbsComponent();
    if (largest > AffineAxisDirectMax || (largest > 0.0 && largest < AffineAxisDirectMin)) {
        return largest * (axis / largest).Length();
    }

    return axis.Length();
}

// 세 Transform의 TryFromMatrix가 같이 쓰는 affine → TRS 분해다(double 계산). 행 벡터 규약이라 행
// 0·1·2가 scale을 곱한 축이다(UE GetScaledAxis). 축 길이를 scale로, 정규화한 축을 회전으로 쓰고
// 행렬식이 음수면 x scale의 부호를 뒤집는다. 회전 축의 직교성 오차가 orthogonalityTolerance를
// 넘으면 shear로 판단해 Unsupported, 비유한 원소·마지막 열이 (0, 0, 0, 1)이 아님·0 길이(또는 비유한
// 길이) 축이면 InvalidArgument다. 축 길이는 극단 크기에서도 overflow·underflow하지
// 않는다(AxisLength).
inline Result<AffineTrsParts> TryDecomposeAffine(const Mat4d& matrix,
                                                 double orthogonalityTolerance) noexcept {
    if (!matrix.IsFinite() || matrix.At(0, 3) != 0.0 || matrix.At(1, 3) != 0.0 ||
        matrix.At(2, 3) != 0.0 || matrix.At(3, 3) != 1.0) {
        return Result<AffineTrsParts>::Failure(ErrorCode::InvalidArgument);
    }

    Vec3d axes[3] = {matrix.GetScaledAxis(0), matrix.GetScaledAxis(1), matrix.GetScaledAxis(2)};
    double lengths[3];
    for (int axis = 0; axis < 3; ++axis) {
        lengths[axis] = AxisLength(axes[axis]);
        if (!(lengths[axis] > 0.0) || !math::IsFinite(lengths[axis])) {
            return Result<AffineTrsParts>::Failure(ErrorCode::InvalidArgument);
        }

        axes[axis] = axes[axis] / lengths[axis];
    }

    if (Dot(Cross(axes[0], axes[1]), axes[2]) < 0.0) {
        lengths[0] = -lengths[0];
        axes[0] = -axes[0];
    }

    if (math::Abs(Dot(axes[0], axes[1])) > orthogonalityTolerance ||
        math::Abs(Dot(axes[0], axes[2])) > orthogonalityTolerance ||
        math::Abs(Dot(axes[1], axes[2])) > orthogonalityTolerance) {
        return Result<AffineTrsParts>::Failure(ErrorCode::Unsupported);
    }

    const Mat4d rotationMatrix = Mat4d::FromRows(Vec4d(axes[0], 0.0), Vec4d(axes[1], 0.0),
                                                 Vec4d(axes[2], 0.0), Vec4d(0.0, 0.0, 0.0, 1.0));
    auto rotation = Quatd::TryFromRotationMatrix(rotationMatrix);
    if (!rotation) {
        return Result<AffineTrsParts>::Failure(rotation.Error());
    }

    return Result<AffineTrsParts>::Success(AffineTrsParts{
        rotation.Value(), matrix.GetTranslation(), Vec3d(lengths[0], lengths[1], lengths[2])});
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
// 정밀도: 점·벡터·방향 변환, 역변환 점·벡터, ToMatrix, TryMultiply의 행렬 곱은 T로
// 계산한다(float 행렬 곱은 platform::simd 경로). TryFromMatrix의 분해는 입력을 double로
// 정확히 넓혀 계산한 뒤 회전·scale을 T로 가장 가깝게 반올림한다(float 행렬을 double로
// 계산하는 Matrix4::TryInverse·Quaternion::TryFromRotationMatrix와 같은 방식). TryInverse도 역
// affine을 double로 직접 만들어 같은 분해를 거친다(행렬식을 쓰지 않으므로 scale 범위가 행렬식의
// 표현 범위에 묶이지 않는다). 그래서 같은 행렬 값의 shear·0 scale 판정은 세 Transform이 같고 결과의
// 표현 범위만 다르다.
//
// rotation은 단위 quaternion이어야 한다. 길이 오차가 IsValid 허용치 안이면 모든 연산이
// 정규화한 회전을 쓴다(UnitRotation — 호출마다 정규화하므로 비용이 든다). 같은 변환을 많은 점에
// 적용할 때는 ToMatrix()를 한 번 만든 뒤 Matrix4::TransformPoint·math::TransformPoints를 쓴다.
// 음수 scale은 허용하며 행렬식 부호로 반사를 표현한다. 값 타입이며 소유하는 자원이 없다.
//
// 사용 예(UE probe와 같은 값 — 자식 (100, 0, 0)을 yaw 90°·이동 (0, 0, 50)·scale 2 부모에 붙인다):
//   const Transform3d parent(Quatd::FromUnitAxisAngle(Vec3d::Up(), math::HalfPi<double>),
//                            Vec3d(0.0, 0.0, 50.0), Vec3d::Splat(2.0));
//   const Transform3d child(Quatd::Identity(), Vec3d(100.0, 0.0, 0.0), Vec3d::One());
//   auto world = Transform3d::TryMultiply(child, parent); // UE Child * Parent
//   if (world) {
//       const Vec3d origin = world.Value().TransformPoint(Vec3d::Zero()); // ≈ (0, 200, 50)
//   }
template <typename T>
struct Transform3 {
    static_assert(math::IsFloatingPointV<T>, "Transform3 supports float and double");

    Quaternion<T> rotation;
    Vector3<T> translation;
    Vector3<T> scale = Vector3<T>::One();

    constexpr Transform3() noexcept = default;

    // UE TTransform(InRotation, InTranslation, InScale3D)와 같은 인자 순서다.
    constexpr Transform3(const Quaternion<T>& rotationValue, const Vector3<T>& translationValue,
                         const Vector3<T>& scaleValue) noexcept
        : rotation(rotationValue), translation(translationValue), scale(scaleValue) {
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

    // 점 변환 rotation.RotateVector(scale · p) + translation이다(UE TransformPosition).
    Vector3<T> TransformPoint(const Vector3<T>& point) const noexcept {
        return UnitRotation().RotateVector(point * scale) + translation;
    }

    // 방향·변위 변환이다. 이동은 적용하지 않고 scale은 적용한다(UE TransformVector).
    Vector3<T> TransformVector(const Vector3<T>& vector) const noexcept {
        return UnitRotation().RotateVector(vector * scale);
    }

    // scale 없이 회전만 적용한다(단위 방향 유지, UE TransformVectorNoScale).
    Vector3<T> TransformDirection(const Vector3<T>& direction) const noexcept {
        return UnitRotation().RotateVector(direction);
    }

    // 역변환 점 변환 R⁻¹(p - t) / s다. 실패(InvalidArgument): scale 성분이 0·비유한이거나 역수가
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

    // 역변환 벡터 변환 R⁻¹v / s다. 실패와 비유한 결과는 TryInverseTransformPoint와 같다.
    Result<Vector3<T>> TryInverseTransformVector(const Vector3<T>& vector) const noexcept {
        if (!detail::HasInvertibleScale(scale)) {
            return Result<Vector3<T>>::Failure(ErrorCode::InvalidArgument);
        }

        return Result<Vector3<T>>::Success(UnitRotation().UnrotateVector(vector) / scale);
    }

    // affine 행렬 S * R * T(행 벡터 — UE ToMatrixWithScale과 같은 원소)를 T 정밀도로 만든다
    // (Transform3f는 Mat4, Transform3d는 Mat4d). 회전 행렬의 행 0·1·2에 scale.x·y·z를 곱하고
    // 마지막 행이 이동이다.
    Matrix4<T> ToMatrix() const noexcept {
        Matrix4<T> result = UnitRotation().ToMatrix();
        const T axisScales[3] = {scale.x, scale.y, scale.z};
        for (int axis = 0; axis < 3; ++axis) {
            for (int column = 0; column < 3; ++column) {
                result.At(axis, column) *= axisScales[axis];
            }
        }

        result.m[12] = translation.x;
        result.m[13] = translation.y;
        result.m[14] = translation.z;
        return result;
    }

    // affine 행렬을 TRS로 분해한다(detail::TryDecomposeAffine — double 계산 뒤 T로 반올림). 회전
    // 축의 직교성 오차가 orthogonalityTolerance를 넘으면 shear로 판단해 Unsupported, 비유한
    // 원소·마지막 열이 (0, 0, 0, 1)이 아님·0 scale(T로 반올림한 뒤 0이나 비유한이 된 경우 포함)이면
    // InvalidArgument다. 반사(행렬식 음수)는 x scale 부호로 표현한다.
    static Result<Transform3> TryFromMatrix(const Matrix4<T>& matrix,
                                            double orthogonalityTolerance = 1.0e-5) noexcept {
        return FromParts(detail::TryDecomposeAffine(Mat4d(matrix), orthogonalityTolerance));
    }

    // first를 먼저, second를 나중에 적용하는 합성이다(UE first * second, Multiply(Out, A, B)).
    // 부모·자식은 TryMultiply(local, parentToWorld)다. TryFromMatrix(first.ToMatrix() *
    // second.ToMatrix())와 같다. shear가 생기면 Unsupported(UE는 shear를 버린다), 비유한값은
    // InvalidArgument다. 0 scale은 곱에 0 길이 축이 남으면 InvalidArgument이고, 0 scale과 회전이
    // 평행한 축을 만들면 TRS로 표현할 수 없어 Unsupported다(어느 쪽인지는 회전 행렬 원소의 반올림에
    // 따라 달라지므로 0 scale은 HasInvertibleScale 등으로 먼저 판정한다). shear를 포함한 정확한
    // affine 합성이 필요하면 first.ToMatrix() * second.ToMatrix()를 직접 쓴다.
    static Result<Transform3> TryMultiply(const Transform3& first, const Transform3& second,
                                          double orthogonalityTolerance = 1.0e-5) noexcept {
        return TryFromMatrix(first.ToMatrix() * second.ToMatrix(), orthogonalityTolerance);
    }

    // 역변환을 TRS로 계산한다. 역 affine(선형 부분 R⁻¹ · S⁻¹, 이동 행 -t · R⁻¹ · S⁻¹)을 double로
    // 만든 뒤 TryFromMatrix와 같은 분해로 T에 반올림한다. 비균일 scale과 회전이 함께 있으면
    // 일반적으로 표현할 수 없어 Unsupported다. 역변환할 수 없는 scale(detail::HasInvertibleScale
    // 위반)과 결과가 T 범위를 넘는 경우(역 이동·역 scale이 비유한)는 InvalidArgument다.
    Result<Transform3> TryInverse(double orthogonalityTolerance = 1.0e-5) const noexcept {
        if (!detail::HasInvertibleScale(scale)) {
            return Result<Transform3>::Failure(ErrorCode::InvalidArgument);
        }

        const Mat4d inverseRotation = Quatd(UnitRotation()).ToMatrix().Transposed();
        const Vec3d axisScale(scale);
        const double axisScales[3] = {axisScale.x, axisScale.y, axisScale.z};
        Mat4d inverse;
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                inverse.At(row, column) = inverseRotation.At(row, column) / axisScales[column];
            }
        }

        const Vec3d moved = inverseRotation.TransformVector(Vec3d(translation)) / axisScale;
        inverse.m[12] = -moved.x;
        inverse.m[13] = -moved.y;
        inverse.m[14] = -moved.z;
        return FromParts(detail::TryDecomposeAffine(inverse, orthogonalityTolerance));
    }

    // Transform3f → Transform3d 넓힘이다. 모든 성분이 정확히 보존된다.
    Transform3<double> ToTransform3d() const noexcept
        requires(IsSameV<T, float>)
    {
        return Transform3<double>(Quaternion<double>(rotation), Vector3<double>(translation),
                                  Vector3<double>(scale));
    }

    // Transform3d → Transform3f 좁힘이다. 모든 성분을 float로 가장 가깝게 반올림한다.
    Transform3<float> RoundToTransform3f() const noexcept
        requires(IsSameV<T, double>)
    {
        return Transform3<float>(Quaternion<float>(rotation), Vector3<float>(translation),
                                 Vector3<float>(scale));
    }

    // Transform(double)으로 바꾼다. float는 정확한 넓힘, double은 같은 값 복사다.
    Transform ToTransform() const noexcept;

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
            Transform3(Quaternion<T>(parts.Value().rotation), roundedTranslation, roundedScale));
    }
};

using Transform3f = Transform3<float>;
using Transform3d = Transform3<double>;

static_assert(sizeof(Transform3f) == 40 && sizeof(Transform3d) == 80);

// 회전·이동·축별 scale(TRS)로 표현한 월드·씬 변환이다(UE FTransform 대응). 모든 성분이 double이고
// 멤버·생성자 순서는 UE와 같이 rotation·translation·scale이다. 규약은 파일 머리 주석과 같다: 점
// 변환은 scale → rotation → translation, TryMultiply(a, b)는 a를 먼저 적용(UE A * B), 부모·자식은
// TryMultiply(local, parentToWorld)다.
//
// 연산은 같은 값의 Transform3d에 맡긴다(결과가 비트 단위로 같다). 비균일 scale 부모와 자식 회전을
// 합성하면 shear가 생겨 TRS로 표현할 수 없다. 이 경우 TryMultiply는 근사하지 않고 Unsupported로
// 실패하며(UE는 shear를 버린다), 정확한 결과가 필요하면 ToMatrix()의 affine 곱(Mat4d)을 쓴다. 0
// scale의 역변환은 InvalidArgument다(UE는 그 축 역수를 0으로 둔다). 음수 scale은 허용하며 행렬식
// 부호로 반사를 표현한다. rotation은 단위 quaternion이어야 한다. 외부 입력은 IsValid로 검사한다.
// 기본값은 항등 변환이다. 많은 수의 본·인스턴스처럼 float로 충분한 대량 경로는 Transform3f를 쓴다.
//
// 사용 예:
//   const Transform relative(Quatd::Identity(), Vec3d(100.0, 0.0, 0.0), Vec3d::One()); // cm
//   auto world = Transform::TryMultiply(relative, parentToWorld);
struct Transform {
    Quatd rotation;
    Vec3d translation;
    Vec3d scale = Vec3d::One();

    constexpr Transform() noexcept = default;

    // UE FTransform(InRotation, InTranslation, InScale3D)와 같은 인자 순서다.
    constexpr Transform(const Quatd& rotationValue, const Vec3d& translationValue,
                        const Vec3d& scaleValue) noexcept
        : rotation(rotationValue), translation(translationValue), scale(scaleValue) {
    }

    static constexpr Transform Identity() noexcept {
        return Transform();
    }

    // 모든 성분이 유한하고 rotation 길이 오차가 rotationTolerance 이하이면 true다.
    bool IsValid(double rotationTolerance = 1.0e-4) const noexcept {
        return ToTransform3d().IsValid(rotationTolerance);
    }

    // 세 축 scale의 절댓값이 같으면(부호는 무관) 균일 scale이다.
    bool HasUniformScale(double tolerance) const noexcept {
        return detail::HasUniformScaleMagnitude(scale, tolerance);
    }

    Vec3d TransformPoint(const Vec3d& point) const noexcept {
        return ToTransform3d().TransformPoint(point);
    }

    // 방향·변위 변환이다. 이동은 적용하지 않고 scale은 적용한다.
    Vec3d TransformVector(const Vec3d& vector) const noexcept {
        return ToTransform3d().TransformVector(vector);
    }

    // scale 없이 회전만 적용한다(단위 방향 유지).
    Vec3d TransformDirection(const Vec3d& direction) const noexcept {
        return ToTransform3d().TransformDirection(direction);
    }

    // 역변환 점 변환 R⁻¹(p - t) / s다. 실패(InvalidArgument): scale 성분 중 0·비유한이거나 역수가
    // double로 유한하지 않음. 결과가 double 범위를 넘을 때만 비유한 성분으로 성공한다.
    Result<Vec3d> TryInverseTransformPoint(const Vec3d& point) const noexcept {
        return ToTransform3d().TryInverseTransformPoint(point);
    }

    Result<Vec3d> TryInverseTransformVector(const Vec3d& vector) const noexcept {
        return ToTransform3d().TryInverseTransformVector(vector);
    }

    // affine 행렬 S * R * T(행 벡터, UE ToMatrixWithScale)를 만든다.
    Mat4d ToMatrix() const noexcept {
        return ToTransform3d().ToMatrix();
    }

    // affine 행렬을 TRS로 분해한다(Transform3d::TryFromMatrix와 같은 판정·결과).
    static Result<Transform> TryFromMatrix(const Mat4d& matrix,
                                           double orthogonalityTolerance = 1.0e-5) noexcept {
        return FromTransform3d(Transform3d::TryFromMatrix(matrix, orthogonalityTolerance));
    }

    // first를 먼저, second를 나중에 적용하는 합성이다(UE first * second). 부모·자식은
    // TryMultiply(local, parentToWorld)다. 실패는 Transform3d::TryMultiply와 같다.
    static Result<Transform> TryMultiply(const Transform& first, const Transform& second,
                                         double orthogonalityTolerance = 1.0e-5) noexcept {
        return FromTransform3d(Transform3d::TryMultiply(
            first.ToTransform3d(), second.ToTransform3d(), orthogonalityTolerance));
    }

    // 역변환을 TRS로 계산한다. 비균일 scale과 회전이 함께 있으면 일반적으로 표현할 수 없어
    // Unsupported다. 0 scale은 InvalidArgument다.
    Result<Transform> TryInverse(double orthogonalityTolerance = 1.0e-5) const noexcept {
        return FromTransform3d(ToTransform3d().TryInverse(orthogonalityTolerance));
    }

    // Transform → Transform3d다. 같은 double 값이라 정확하다.
    constexpr Transform3d ToTransform3d() const noexcept {
        return Transform3d(rotation, translation, scale);
    }

    // Transform → Transform3f 좁힘이다. 모든 성분을 float로 가장 가깝게 반올림한다. 원점에서 먼
    // 이동은 float 간격으로 양자화된다(1e9 근처는 64 단위) — 카메라·타일 원점을 뺀 상대 값을
    // 좁힌다.
    Transform3f RoundToTransform3f() const noexcept {
        return ToTransform3d().RoundToTransform3f();
    }

private:
    static Result<Transform> FromTransform3d(const Result<Transform3d>& value) noexcept {
        if (!value) {
            return Result<Transform>::Failure(value.Error());
        }

        return Result<Transform>::Success(value.Value().ToTransform());
    }
};

template <typename T>
Transform Transform3<T>::ToTransform() const noexcept {
    return Transform(Quatd(rotation), Vec3d(translation), Vec3d(scale));
}

} // namespace rocks
