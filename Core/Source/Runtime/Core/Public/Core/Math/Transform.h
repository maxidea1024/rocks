#pragma once

#include <Core/Math/Quaternion.h>

namespace rocks {

// 이동·회전·축별 scale(TRS)로 표현한 authoring 변환이다. 설계 규약(§2.3)에 따라 이동은
// double(Vec3d), 회전은 float quaternion, scale은 float이다. 계산은 모두 double로 수행한다.
// 점 변환 순서는 scale → rotation → translation: p' = T * R * S * p.
// 합성 규약은 world = parent * local이다. UE FTransform의 A * B(A 먼저 적용)와 순서가 반대다.
//
// 비균일 parent scale과 child 회전을 합성하면 shear가 생겨 TRS로 표현할 수 없다. 이 경우
// TryCompose는 근사하지 않고 Unsupported로 실패하며, 정확한 결과가 필요하면 ToMatrix()의
// affine 곱(Mat4d)을 사용한다. 음수 scale은 허용하며 행렬식 부호로 반사를 표현한다.
// rotation은 단위 quaternion이어야 한다. 외부 입력은 IsValid로 검사한다. 기본값은 항등 변환이다.
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
        const float ax = math::Abs(scale.x);
        return math::Abs(math::Abs(scale.y) - ax) <= tolerance &&
               math::Abs(math::Abs(scale.z) - ax) <= tolerance;
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
        if (!matrix.IsFinite() || matrix.At(3, 0) != 0.0 || matrix.At(3, 1) != 0.0 ||
            matrix.At(3, 2) != 0.0 || matrix.At(3, 3) != 1.0) {
            return Result<Transform>::Failure(ErrorCode::InvalidArgument);
        }
        Vec3d columns[3] = {matrix.Column(0).Xyz(), matrix.Column(1).Xyz(), matrix.Column(2).Xyz()};
        double lengths[3];
        for (int axis = 0; axis < 3; ++axis) {
            lengths[axis] = columns[axis].Length();
            if (!(lengths[axis] > 0.0) || !math::IsFinite(lengths[axis])) {
                return Result<Transform>::Failure(ErrorCode::InvalidArgument);
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
            return Result<Transform>::Failure(ErrorCode::Unsupported);
        }
        const Mat4d rotationMatrix =
            Mat4d::FromColumns(Vec4d(columns[0], 0.0), Vec4d(columns[1], 0.0),
                               Vec4d(columns[2], 0.0), Vec4d(0.0, 0.0, 0.0, 1.0));
        auto rotation = Quatd::TryFromRotationMatrix(rotationMatrix);
        if (!rotation) {
            return Result<Transform>::Failure(rotation.Error());
        }
        const Vec3 scale(static_cast<float>(lengths[0]), static_cast<float>(lengths[1]),
                         static_cast<float>(lengths[2]));
        if (!scale.IsFinite() || scale.x == 0.0f || scale.y == 0.0f || scale.z == 0.0f) {
            return Result<Transform>::Failure(ErrorCode::InvalidArgument);
        }
        return Result<Transform>::Success(
            Transform(matrix.GetTranslation(), Quat(rotation.Value()), scale));
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

    bool HasInvertibleScale() const noexcept {
        return scale.IsFinite() && scale.x != 0.0f && scale.y != 0.0f && scale.z != 0.0f;
    }
};

} // namespace rocks
