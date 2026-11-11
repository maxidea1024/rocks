#pragma once

#include <Core/Math/Quaternion.h>

namespace rocks {

// 자주 쓰는 4x4 행렬 생성 함수 모음이다(UE의 Perspective/ReversedZPerspective/Ortho/
// ReversedZOrtho/LookAt/LookFrom/Mirror/RotationAboutPoint/ScaleRotationTranslation/
// Rotation/InverseRotation/Translation/Scale/ClipProjection 행렬에 대응).
//
// 공통 규약:
// - column vector, p' = M * p. 합성은 world = parent * local.
// - Rocks view 공간: 카메라가 +X를 바라보고 +Y가 왼쪽, +Z가 위다(월드 축 규약과 같다).
//   UE view 공간(+Z 전방, +X 오른쪽, +Y 위, row vector)과 다르므로 행렬을 그대로 교환하지 않는다.
// - NDC: x 오른쪽, y 위, z는 DepthRange에 따른 [0, 1]. 그래픽 API별 y 반전과 clip 규약 차이는
//   RHI 경계에서 한 번만 적용한다.
// - 입력 오류는 Try* 함수가 InvalidArgument로 반환하며 부분 결과를 만들지 않는다.

// 깊이 매핑이다. Reversed는 near → 1, far → 0으로 부동소수점 정밀도를 먼 거리에 고르게 배분한다.
// Rocks 렌더 기본 규약은 Reversed이다.
enum class DepthRange : uint8 {
    Standard, // near → 0, far → 1
    Reversed  // near → 1, far → 0
};

namespace detail {

// view 깊이 d(= view x)에 대한 clip z = scale * d + offset 계수다. 원근 투영은 w = d다.
// n * f / (f - n)은 n * f가 먼저 overflow할 수 있으므로 n * (f / (f - n))으로 계산한다.
template <typename T>
inline void PerspectiveDepth(T nearDistance, T farDistance, DepthRange depth, T& scale,
                             T& offset) noexcept {
    const bool infinite = !math::IsFinite(farDistance);
    const T farRatio = infinite ? static_cast<T>(1) : farDistance / (farDistance - nearDistance);
    if (depth == DepthRange::Reversed) {
        scale = infinite ? static_cast<T>(0) : nearDistance / (nearDistance - farDistance);
        offset = nearDistance * farRatio;
    } else {
        scale = farRatio;
        offset = -nearDistance * farRatio;
    }
}

template <typename T>
inline bool IsValidDepth(T nearDistance, T farDistance) noexcept {
    return nearDistance > static_cast<T>(0) && math::IsFinite(nearDistance) &&
           farDistance > nearDistance;
}

template <typename T>
inline Result<Matrix4<T>> FiniteOrInvalid(const Matrix4<T>& matrix) noexcept {
    if (!matrix.IsFinite()) {
        return Result<Matrix4<T>>::Failure(ErrorCode::InvalidArgument);
    }
    return Result<Matrix4<T>>::Success(matrix);
}

} // namespace detail

// ---- 투영 ----

// 비대칭 원근 투영이다. left/right/bottom/top은 near 평면 위의 화면 좌표(오른쪽·위가 양수)이다.
// farDistance = +Inf면 무한 원거리 투영이다.
// 실패: left >= right, bottom >= top, 비유한 경계, near <= 0, far <= near, NaN.
template <typename T>
Result<Matrix4<T>> TryMakePerspectiveOffCenter(T left, T right, T bottom, T top, T nearDistance,
                                               T farDistance,
                                               DepthRange depth = DepthRange::Reversed) noexcept {
    if (!(left < right) || !(bottom < top) || !math::IsFinite(left) || !math::IsFinite(right) ||
        !math::IsFinite(bottom) || !math::IsFinite(top) ||
        !detail::IsValidDepth(nearDistance, farDistance)) {
        return Result<Matrix4<T>>::Failure(ErrorCode::InvalidArgument);
    }
    const T width = right - left;
    const T height = top - bottom;
    T depthScale = 0;
    T depthOffset = 0;
    detail::PerspectiveDepth(nearDistance, farDistance, depth, depthScale, depthOffset);
    Matrix4<T> result = Matrix4<T>::Zero();
    // 화면 x = -view y, 화면 y = view z, w = view x(전방 거리).
    result.At(0, 0) = -(right + left) / width;
    result.At(0, 1) = -static_cast<T>(2) * nearDistance / width;
    result.At(1, 0) = -(top + bottom) / height;
    result.At(1, 2) = static_cast<T>(2) * nearDistance / height;
    result.At(2, 0) = depthScale;
    result.At(2, 3) = depthOffset;
    result.At(3, 0) = static_cast<T>(1);
    return detail::FiniteOrInvalid(result);
}

// 대칭 원근 투영이다. verticalFovRadians는 전체 수직 화각(0, pi), aspectRatio = 폭 / 높이.
// UE의 Perspective/ReversedZPerspective(HalfFOV, Width, Height, MinZ[, MaxZ])와 같은 용도이며
// UE는 반화각·수평 기준 FOV를 받는 점이 다르다.
template <typename T>
Result<Matrix4<T>> TryMakePerspective(T verticalFovRadians, T aspectRatio, T nearDistance,
                                      T farDistance,
                                      DepthRange depth = DepthRange::Reversed) noexcept {
    if (!(verticalFovRadians > static_cast<T>(0) && verticalFovRadians < math::Pi<T>) ||
        !(aspectRatio > static_cast<T>(0)) || !math::IsFinite(aspectRatio) ||
        !detail::IsValidDepth(nearDistance, farDistance)) {
        return Result<Matrix4<T>>::Failure(ErrorCode::InvalidArgument);
    }
    const T halfHeight = nearDistance * math::Tan(verticalFovRadians * static_cast<T>(0.5));
    const T halfWidth = halfHeight * aspectRatio;
    return TryMakePerspectiveOffCenter(-halfWidth, halfWidth, -halfHeight, halfHeight, nearDistance,
                                       farDistance, depth);
}

// 비대칭 직교 투영이다. 경계는 view 공간 화면 좌표(오른쪽·위 양수), 깊이는 전방 거리다.
// near는 0 이상(카메라 위치 포함)이고 far는 유한해야 한다. w = 1이다.
template <typename T>
Result<Matrix4<T>> TryMakeOrthographicOffCenter(T left, T right, T bottom, T top, T nearDistance,
                                                T farDistance,
                                                DepthRange depth = DepthRange::Reversed) noexcept {
    if (!(left < right) || !(bottom < top) || !math::IsFinite(left) || !math::IsFinite(right) ||
        !math::IsFinite(bottom) || !math::IsFinite(top) || !(nearDistance >= static_cast<T>(0)) ||
        !math::IsFinite(farDistance) || !(farDistance > nearDistance)) {
        return Result<Matrix4<T>>::Failure(ErrorCode::InvalidArgument);
    }
    const T width = right - left;
    const T height = top - bottom;
    const T range = farDistance - nearDistance;
    Matrix4<T> result = Matrix4<T>::Zero();
    result.At(0, 1) = -static_cast<T>(2) / width;
    result.At(0, 3) = -(right + left) / width;
    result.At(1, 2) = static_cast<T>(2) / height;
    result.At(1, 3) = -(top + bottom) / height;
    if (depth == DepthRange::Reversed) {
        result.At(2, 0) = -static_cast<T>(1) / range;
        result.At(2, 3) = farDistance / range;
    } else {
        result.At(2, 0) = static_cast<T>(1) / range;
        result.At(2, 3) = -nearDistance / range;
    }
    result.At(3, 3) = static_cast<T>(1);
    return detail::FiniteOrInvalid(result);
}

// 대칭 직교 투영이다. width/height는 view 공간 전체 폭·높이(미터)다.
template <typename T>
Result<Matrix4<T>> TryMakeOrthographic(T width, T height, T nearDistance, T farDistance,
                                       DepthRange depth = DepthRange::Reversed) noexcept {
    const T halfWidth = width * static_cast<T>(0.5);
    const T halfHeight = height * static_cast<T>(0.5);
    return TryMakeOrthographicOffCenter(-halfWidth, halfWidth, -halfHeight, halfHeight,
                                        nearDistance, farDistance, depth);
}

// 투영 행렬의 near 평면을 view 공간 평면으로 바꾼 oblique clip 투영이다(UE ClipProjection 대응,
// Lengyel의 oblique near-plane 방법). planeNormal·planeDistance는 view 공간에서
// Dot(normal, p) = distance를 만족하는 평면이고, 카메라는 평면의 음수 쪽(보이는 쪽은 양수)에
// 있어야 한다. depth는 projection을 만들 때 쓴 DepthRange와 같아야 한다(행렬만으로는 구분할 수
// 없으므로 기본값 없이 명시한다). 원근 투영을 대상으로 검증했다.
// - Standard: near(z/w = 0)를 평면으로 바꾸고 far 평면(z = w)이 반대쪽 frustum 모서리를 지나게
// 한다.
// - Reversed: near(z/w = 1, 즉 w - z = 0)를 평면으로 바꾸고 far 평면(z = 0)이 모서리를 지나게 한다.
// 실패: projection 비유한·역행렬 없음, 평면 비유한·영 법선, 카메라가 평면 위/양수 쪽.
template <typename T>
Result<Matrix4<T>> TryMakeObliqueClipProjection(const Matrix4<T>& projection,
                                                const Vector3<T>& planeNormal, T planeDistance,
                                                DepthRange depth) noexcept {
    auto normal = planeNormal.TryNormalize();
    if (!projection.IsFinite() || !normal || !math::IsFinite(planeDistance)) {
        return Result<Matrix4<T>>::Failure(ErrorCode::InvalidArgument);
    }
    // 동차 평면 c = (n, -d). 카메라(원점)가 평면 음수 쪽이면 c·(0,0,0,1) = -d < 0.
    // 원래 법선 길이 = Dot(원래, 단위). 제곱 길이 overflow 없이 distance를 같은 평면으로 맞춘다.
    const T normalLength = Dot(planeNormal, normal.Value());
    const Vector4<T> clipPlane(normal.Value(), -planeDistance / normalLength);
    if (!(clipPlane.w < static_cast<T>(0))) {
        return Result<Matrix4<T>>::Failure(ErrorCode::InvalidArgument);
    }
    auto inverse = projection.TryInverse();
    if (!inverse) {
        return Result<Matrix4<T>>::Failure(inverse.Error());
    }
    // clip 공간의 반대쪽 frustum 모서리 q = M⁻¹ (sgn(c'.x), sgn(c'.y), far z, 1).
    // far z는 Standard에서 1, Reversed에서 0이다.
    const bool reversed = depth == DepthRange::Reversed;
    const Matrix4<T> inverseTranspose = inverse.Value().Transposed();
    const Vector4<T> clipSpacePlane = inverseTranspose * clipPlane;
    const Vector4<T> corner(math::Sign(clipSpacePlane.x), math::Sign(clipSpacePlane.y),
                            reversed ? static_cast<T>(0) : static_cast<T>(1), static_cast<T>(1));
    const Vector4<T> q = inverse.Value() * corner;
    const T denominator = Dot(clipPlane, q);
    if (!(math::Abs(denominator) > static_cast<T>(0))) {
        return Result<Matrix4<T>>::Failure(ErrorCode::InvalidArgument);
    }
    // a = (row4 · q) / (c · q).
    // Standard: 새 세 번째 행 = a * c (near z = 0이 평면 c, far z = w가 q를 지난다).
    // Reversed: 새 세 번째 행 = row4 - a * c (near w - z = a * c, far z = 0이 q를 지난다).
    const Vector4<T> row4 = projection.Row(3);
    const Vector4<T> scaled = clipPlane * (Dot(row4, q) / denominator);
    const Vector4<T> row3 = reversed ? row4 - scaled : scaled;
    Matrix4<T> result = projection;
    result.At(2, 0) = row3.x;
    result.At(2, 1) = row3.y;
    result.At(2, 2) = row3.z;
    result.At(2, 3) = row3.w;
    return detail::FiniteOrInvalid(result);
}

// ---- view ----

// LookTo가 up과 forward 사이 각의 sin으로 요구하는 최솟값이다. 두 단위 벡터의 외적은 반올림으로
// 약 수 ULP의 길이를 가지므로 정확히 0인지로는 평행을 판정할 수 없다. 64 * MachineEpsilon은
// float에서 약 7.6e-6 rad(0.0004도), double에서 약 1.4e-14 rad이다.
template <typename T>
    requires math::IsFloatingPointV<T>
inline constexpr T LookToMinimumSine = static_cast<T>(64) * math::MachineEpsilon<T>;

// eye에서 forward 방향을 보는 world → view 행렬이다(UE LookFrom 대응). up은 대략적인 위 방향이며
// forward와 (거의) 평행하면(sin < LookToMinimumSine) 실패한다. view 축: +X = forward,
// +Y = up × forward(왼쪽), +Z = 정확한 위. UE LookFromMatrix는 평행 판정이 없다.
template <typename T>
Result<Matrix4<T>> TryMakeLookTo(const Vector3<T>& eye, const Vector3<T>& forward,
                                 const Vector3<T>& up) noexcept {
    auto axisX = forward.TryNormalize();
    auto unitUp = up.TryNormalize();
    if (!eye.IsFinite() || !axisX || !unitUp) {
        return Result<Matrix4<T>>::Failure(ErrorCode::InvalidArgument);
    }
    // 두 단위 벡터의 외적 길이는 sin이다.
    auto axisY = Cross(unitUp.Value(), axisX.Value()).TryNormalize(LookToMinimumSine<T>);
    if (!axisY) {
        return Result<Matrix4<T>>::Failure(ErrorCode::InvalidArgument);
    }
    const Vector3<T> axisZ = Cross(axisX.Value(), axisY.Value());
    // 회전 부분의 행은 view 축이고, 이동은 -R * eye다.
    const Matrix4<T> result = Matrix4<T>::FromRows(
        Vector4<T>(axisX.Value(), -Dot(axisX.Value(), eye)),
        Vector4<T>(axisY.Value(), -Dot(axisY.Value(), eye)), Vector4<T>(axisZ, -Dot(axisZ, eye)),
        Vector4<T>(static_cast<T>(0), static_cast<T>(0), static_cast<T>(0), static_cast<T>(1)));
    return detail::FiniteOrInvalid(result);
}

// eye에서 target을 보는 world → view 행렬이다(UE LookAt 대응). eye == target이면 실패한다.
template <typename T>
Result<Matrix4<T>> TryMakeLookAt(const Vector3<T>& eye, const Vector3<T>& target,
                                 const Vector3<T>& up) noexcept {
    return TryMakeLookTo(eye, target - eye, up);
}

// ---- 기본 affine ----

// 축 회전 행렬이다. 오른손 법칙: 축을 향해 내려다볼 때 양의 각은 반시계 방향.
template <typename T>
Matrix4<T> MakeRotationX(T radians) noexcept {
    const T sine = math::Sin(radians);
    const T cosine = math::Cos(radians);
    Matrix4<T> result;
    result.At(1, 1) = cosine;
    result.At(1, 2) = -sine;
    result.At(2, 1) = sine;
    result.At(2, 2) = cosine;
    return result;
}

template <typename T>
Matrix4<T> MakeRotationY(T radians) noexcept {
    const T sine = math::Sin(radians);
    const T cosine = math::Cos(radians);
    Matrix4<T> result;
    result.At(0, 0) = cosine;
    result.At(0, 2) = sine;
    result.At(2, 0) = -sine;
    result.At(2, 2) = cosine;
    return result;
}

template <typename T>
Matrix4<T> MakeRotationZ(T radians) noexcept {
    const T sine = math::Sin(radians);
    const T cosine = math::Cos(radians);
    Matrix4<T> result;
    result.At(0, 0) = cosine;
    result.At(0, 1) = -sine;
    result.At(1, 0) = sine;
    result.At(1, 1) = cosine;
    return result;
}

// 단위 quaternion 회전 행렬과 그 역(전치)이다(UE Rotation/InverseRotation 대응).
template <typename T>
Matrix4<T> MakeRotation(const Quaternion<T>& rotation) noexcept {
    return rotation.ToMatrix();
}

template <typename T>
Matrix4<T> MakeInverseRotation(const Quaternion<T>& rotation) noexcept {
    return rotation.ToMatrix().Transposed();
}

template <typename T>
Matrix4<T> MakeTranslation(const Vector3<T>& translation) noexcept {
    return Matrix4<T>::Translation(translation);
}

template <typename T>
Matrix4<T> MakeScale(const Vector3<T>& scale) noexcept {
    return Matrix4<T>::Scale(scale);
}

// T * R(이동·회전) 행렬이다(UE RotationTranslation/QuatRotationTranslation 대응).
template <typename T>
Matrix4<T> MakeRotationTranslation(const Quaternion<T>& rotation,
                                   const Vector3<T>& translation) noexcept {
    return MakeRotationTranslationMatrix(rotation, translation);
}

// T * R * S 행렬이다(UE ScaleRotationTranslation 대응). 점에는 scale → 회전 → 이동 순서로 적용된다.
template <typename T>
Matrix4<T> MakeTranslationRotationScale(const Vector3<T>& translation,
                                        const Quaternion<T>& rotation,
                                        const Vector3<T>& scale) noexcept {
    Matrix4<T> result = rotation.ToMatrix();
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

// pivot을 중심으로 회전하는 행렬 T(pivot) * R * T(-pivot)이다(UE RotationAboutPoint 대응).
template <typename T>
Matrix4<T> MakeRotationAboutPoint(const Quaternion<T>& rotation, const Vector3<T>& pivot) noexcept {
    const Vector3<T> offset = pivot - rotation.RotateVector(pivot);
    return MakeRotationTranslationMatrix(rotation, offset);
}

// Dot(normal, p) = distance 평면에 대한 반사 행렬 I - 2nnᵀ, 이동 2dn이다(UE Mirror 대응).
// 행렬식은 -1이므로 삼각형 winding이 뒤집힌다. 실패: 영·비유한 법선, 비유한 distance.
template <typename T>
Result<Matrix4<T>> TryMakeMirror(const Vector3<T>& planeNormal, T planeDistance) noexcept {
    auto normal = planeNormal.TryNormalize();
    if (!normal || !math::IsFinite(planeDistance)) {
        return Result<Matrix4<T>>::Failure(ErrorCode::InvalidArgument);
    }
    // 정규화 전 길이로 distance도 같은 평면이 되게 나눈다.
    const T distance = planeDistance / Dot(planeNormal, normal.Value());
    const Vector3<T> n = normal.Value();
    const T components[3] = {n.x, n.y, n.z};
    Matrix4<T> result;
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            const T identity = row == column ? static_cast<T>(1) : static_cast<T>(0);
            result.At(row, column) =
                identity - static_cast<T>(2) * components[row] * components[column];
        }
        result.At(row, 3) = static_cast<T>(2) * distance * components[row];
    }
    return detail::FiniteOrInvalid(result);
}

} // namespace rocks
