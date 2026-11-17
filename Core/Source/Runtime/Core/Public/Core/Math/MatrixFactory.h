#pragma once

#include <Core/Math/Rotator.h>

namespace rocks {

// 자주 쓰는 4x4 행렬 생성 함수 모음이다(UE 5.8 Perspective/ReversedZPerspective/Ortho/
// ReversedZOrtho/LookFrom/LookAt/Mirror/RotationAboutPoint/ScaleRotationTranslation/
// QuatRotation(Translation)/InverseRotation/Translation/Scale/ClipProjection 행렬에 대응 —
// Core/Public/Math/의 같은 이름 헤더).
//
// 공통 규약(Matrix4.h와 같다):
// - 행 벡터 규약 p' = p · M, 합성 A * B는 A를 먼저 적용한다(LocalToWorld = Local * ParentToWorld).
// - view 공간은 UE와 같다: +X 오른쪽, +Y 위, +Z 전방(카메라가 보는 쪽, 깊이). world → view는
//   T(-origin) * MakeViewRotation(cameraRotation)이다(UE SceneView.cpp:684·LocalPlayer.cpp:
//   1241-1245).
// - NDC: x 오른쪽, y 위, z는 DepthRange에 따른 [0, 1]. 그래픽 API별 y 반전과 clip 규약 차이는
//   RHI 경계에서 한 번만 적용한다.
// - 길이 인자(near·far·폭·눈 위치)의 단위는 호출자의 월드 단위(Rocks는 cm)이며 행렬은 단위를
//   바꾸지 않는다.
// - 입력 오류는 Try* 함수가 InvalidArgument로 반환하며 부분 결과를 만들지 않는다(UE 생성자는
//   검사하지 않고 0 나눗셈·NaN 행렬을 만들 수 있다).
//
// UE에 있는 축별 회전 생성 함수는 없다(UE에도 X/Y/Z 축 회전 행렬 타입이 없다). 축 회전은
// MakeRotation(Quaternion::FromUnitAxisAngle(axis, radians))로 만든다.

// 깊이 매핑이다. Reversed는 near → 1, far → 0으로 부동소수점 정밀도를 먼 거리에 고르게 배분한다.
// Rocks·UE 렌더 기본 규약은 Reversed이다(UE FReversedZPerspectiveMatrix·FReversedZOrthoMatrix).
// Standard는 UE FPerspectiveMatrix·FOrthoMatrix에 대응한다.
enum class DepthRange : uint8 {
    Standard, // near → 0, far → 1
    Reversed  // near → 1, far → 0
};

namespace detail {

// view 깊이 d(= view z)에 대한 clip z = scale * d + offset 계수다. 원근 투영은 w = d다.
// Standard는 UE FPerspectiveMatrix 식 그대로(scale = f / (f - n), offset = -n * (f / (f - n))),
// Reversed의 scale은 UE 식 그대로 n / (n - f)다. Reversed offset은 UE -f * n / (n - f)와 같은 값을
// n * (f / (f - n))으로 계산한다(n * f가 먼저 overflow하지 않게 — 결과는 반올림 수준에서 다를 수
// 있다). far가 +Inf면 무한 원거리다(UE 4인자 생성자: Reversed scale 0·offset n, Standard는 UE의
// 1 - Z_PRECISION 대신 정확한 극한 scale 1·offset -n).
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
inline bool IsPositiveFinite(T value) noexcept {
    return value > static_cast<T>(0) && math::IsFinite(value);
}

template <typename T>
inline Result<Matrix4<T>> FiniteOrInvalid(const Matrix4<T>& matrix) noexcept {
    if (!matrix.IsFinite()) {
        return Result<Matrix4<T>>::Failure(ErrorCode::InvalidArgument);
    }
    return Result<Matrix4<T>>::Success(matrix);
}

// 카메라 로컬 축(+X 전방, +Y 오른쪽, +Z 위)을 view 축(+X 오른쪽, +Y 위, +Z 전방)으로 바꾸는
// 행렬이다. 행은 (0,0,1,0), (1,0,0,0), (0,1,0,0), (0,0,0,1)이다(UE LocalPlayer.cpp:1241-1245,
// SceneView.cpp:819-823).
template <typename T>
constexpr Matrix4<T> ViewAxisSwizzle() noexcept {
    const T zero = static_cast<T>(0);
    const T one = static_cast<T>(1);
    return Matrix4<T>::FromRows(
        Vector4<T>(zero, zero, one, zero), Vector4<T>(one, zero, zero, zero),
        Vector4<T>(zero, one, zero, zero), Vector4<T>(zero, zero, zero, one));
}

} // namespace detail

// ---- 투영 ----

// 대칭 원근 투영이다(UE FReversedZPerspectiveMatrix(HalfFOV, Width, Height, MinZ, MaxZ) — 기본
// Reversed, Standard는 UE FPerspectiveMatrix — PerspectiveMatrix.h:101-109·:134-142).
// - halfFovRadians: 수평 반화각(라디안, (0, π/2)). UE 카메라는 수평 전체 FOV(도)를
//   FOV·π/360으로 넘긴다(CameraStackTypes.cpp:212-216). UE 헤더 주석은 "Y direction"이라 쓰지만
//   식은 view x(수평)에 1/tan(HalfFOV)를 둔다.
// - width·height: 화면 비율만 쓴다(view y 계수 = width / tan(HalfFOV) / height). UE 카메라는
//   (AspectRatio, 1)을 넘긴다.
// - minZ·maxZ: near·far 평면까지의 view 깊이. maxZ = +Inf면 무한 원거리다(UE 4인자 생성자 —
//   아래 오버로드).
// 결과: M[0][0] = 1 / tan(h), M[1][1] = width / tan(h) / height, M[2][3] = 1(w = view z),
// M[2][2]·M[3][2] = detail::PerspectiveDepth. view x·y·z가 NDC x 오른쪽·y 위·깊이로 간다.
// 실패(InvalidArgument): 반화각이 (0, π/2) 밖·NaN, width·height가 양의 유한수가 아님, minZ <= 0·
// 비유한, maxZ <= minZ·NaN, 결과에 비유한 원소.
//
// 사용 예(UE 기본 카메라 — 수평 90°, 16:9, near 10 cm, 무한 원거리):
//   auto projection = TryMakePerspective(math::DegreesToRadians(90.0) * 0.5, 16.0, 9.0, 10.0);
template <typename T>
Result<Matrix4<T>> TryMakePerspective(T halfFovRadians, T width, T height, T minZ, T maxZ,
                                      DepthRange depth = DepthRange::Reversed) noexcept {
    if (!(halfFovRadians > static_cast<T>(0) && halfFovRadians < math::HalfPi<T>) ||
        !detail::IsPositiveFinite(width) || !detail::IsPositiveFinite(height) ||
        !detail::IsValidDepth(minZ, maxZ)) {
        return Result<Matrix4<T>>::Failure(ErrorCode::InvalidArgument);
    }
    T depthScale = 0;
    T depthOffset = 0;
    detail::PerspectiveDepth(minZ, maxZ, depth, depthScale, depthOffset);
    Matrix4<T> result = Matrix4<T>::Zero();
    result.At(0, 0) = static_cast<T>(1) / math::Tan(halfFovRadians);
    result.At(1, 1) = width / math::Tan(halfFovRadians) / height;
    result.At(2, 2) = depthScale;
    result.At(2, 3) = static_cast<T>(1);
    result.At(3, 2) = depthOffset;
    return detail::FiniteOrInvalid(result);
}

// 무한 원거리 대칭 원근 투영이다(UE FReversedZPerspectiveMatrix(HalfFOV, Width, Height, MinZ) —
// PerspectiveMatrix.h:145-153, UE 카메라 기본 투영). 위 함수에 maxZ = +Inf를 준 것과 같다.
template <typename T>
Result<Matrix4<T>> TryMakePerspective(T halfFovRadians, T width, T height, T minZ,
                                      DepthRange depth = DepthRange::Reversed) noexcept {
    return TryMakePerspective(halfFovRadians, width, height, minZ,
                              platform::PlatformMath::Infinity<T>(), depth);
}

// 비대칭 원근 투영이다. left/right는 near 평면 위의 view x 경계(오른쪽이 양수), bottom/top은 view y
// 경계(위가 양수)다. 결과는 M[0][0] = 2n / (r - l), M[2][0] = -(r + l) / (r - l), M[1][1] =
// 2n / (t - b), M[2][1] = -(t + b) / (t - b)이고 깊이 열은 대칭 투영과 같다. UE는 대칭 행렬의
// M[2][0]·M[2][1]을 같은 식으로 바꿔 off-center를 만든다(CameraStackTypes.cpp:219-235 — NDC 단위
// 경계). farDistance = +Inf면 무한 원거리다.
// 실패(InvalidArgument): left >= right, bottom >= top, 비유한 경계, near <= 0, far <= near, NaN.
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
    result.At(0, 0) = static_cast<T>(2) * nearDistance / width;
    result.At(1, 1) = static_cast<T>(2) * nearDistance / height;
    result.At(2, 0) = -(right + left) / width;
    result.At(2, 1) = -(top + bottom) / height;
    result.At(2, 2) = depthScale;
    result.At(2, 3) = static_cast<T>(1);
    result.At(3, 2) = depthOffset;
    return detail::FiniteOrInvalid(result);
}

// 대칭 직교 투영이다(UE FReversedZOrthoMatrix(Width, Height, ZScale, ZOffset) — 기본 Reversed,
// Standard는 UE FOrthoMatrix — OrthoMatrix.h:53-72). UE의 Width·Height는 반폭·반높이이며(M[0][0] =
// 1 / Width), UE 카메라는 ZScale = 1 / (far - near), ZOffset = -near로 부른다
// (CameraStackTypes.cpp:190-205). 이 함수는 그 호출과 같은 식으로 계산한다: M[0][0] = 1 /
// halfWidth, M[1][1] = 1 / halfHeight, Reversed M[2][2] = -ZScale·M[3][2] = 1 - ZOffset·ZScale,
// Standard M[2][2] = ZScale·M[3][2] = ZOffset·ZScale, M[3][3] = 1(w = 1). near는 0 이상(카메라 위치
// 포함)이고 far는 유한해야 한다. 실패(InvalidArgument): halfWidth·halfHeight가 양의 유한수가
// 아님(UE는 0이면 1을 쓴다), near < 0· 비유한, far <= near·비유한.
template <typename T>
Result<Matrix4<T>> TryMakeOrthographic(T halfWidth, T halfHeight, T nearDistance, T farDistance,
                                       DepthRange depth = DepthRange::Reversed) noexcept {
    if (!detail::IsPositiveFinite(halfWidth) || !detail::IsPositiveFinite(halfHeight) ||
        !(nearDistance >= static_cast<T>(0)) || !math::IsFinite(farDistance) ||
        !(farDistance > nearDistance)) {
        return Result<Matrix4<T>>::Failure(ErrorCode::InvalidArgument);
    }
    const T zScale = static_cast<T>(1) / (farDistance - nearDistance);
    const T zOffset = -nearDistance;
    Matrix4<T> result = Matrix4<T>::Zero();
    result.At(0, 0) = static_cast<T>(1) / halfWidth;
    result.At(1, 1) = static_cast<T>(1) / halfHeight;
    if (depth == DepthRange::Reversed) {
        result.At(2, 2) = -zScale;
        result.At(3, 2) = static_cast<T>(1) - zOffset * zScale;
    } else {
        result.At(2, 2) = zScale;
        result.At(3, 2) = zOffset * zScale;
    }
    result.At(3, 3) = static_cast<T>(1);
    return detail::FiniteOrInvalid(result);
}

// 비대칭 직교 투영이다. 경계는 view x(오른쪽 양수)·view y(위 양수), 깊이는 view z다.
// x ∈ [left, right] → NDC [-1, 1]: M[0][0] = 2 / (r - l), M[3][0] = -(r + l) / (r - l)(y도 같다).
// 깊이: Reversed M[2][2] = -1 / (f - n)·M[3][2] = f / (f - n), Standard M[2][2] = 1 / (f - n)·
// M[3][2] = -n / (f - n). UE 6인자 FReversedZOrthoMatrix(Left, Right, …)는 M[0][0] = 1 / (R - L)로
// 다른 정규화를 쓰므로 대응시키지 않는다.
// 실패(InvalidArgument): left >= right, bottom >= top, 비유한 경계, near < 0, far <= near·비유한.
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
    result.At(0, 0) = static_cast<T>(2) / width;
    result.At(1, 1) = static_cast<T>(2) / height;
    result.At(3, 0) = -(right + left) / width;
    result.At(3, 1) = -(top + bottom) / height;
    if (depth == DepthRange::Reversed) {
        result.At(2, 2) = -static_cast<T>(1) / range;
        result.At(3, 2) = farDistance / range;
    } else {
        result.At(2, 2) = static_cast<T>(1) / range;
        result.At(3, 2) = -nearDistance / range;
    }
    result.At(3, 3) = static_cast<T>(1);
    return detail::FiniteOrInvalid(result);
}

// 투영 행렬의 near 평면을 view 공간 평면으로 바꾼 oblique clip 투영이다(UE ClipProjection 대응 —
// ClipProjectionMatrix.h:44-68, Lengyel의 oblique near-plane 방법). planeNormal·planeDistance는
// view 공간(+Z 전방)에서 Dot(normal, p) = distance를 만족하는 평면이고(UE FPlane Ax+By+Cz=D와 같은
// 부호), 카메라는 평면의 음수 쪽(보이는 쪽은 양수)에 있어야 한다. depth는 projection을 만들 때 쓴
// DepthRange와 같아야 하며 기본값 없이 명시한다. 새 clip z 열(M[*][2])만 바뀐다.
// - Standard: near(z/w = 0)를 평면으로 바꾸고 far 평면(z = w)이 반대쪽 frustum 모서리를 지나게
//   한다(UE ClipProjection과 같은 결과 — UE는 Standard만 다룬다).
// - Reversed: near(z/w = 1, 즉 w - z = 0)를 평면으로 바꾸고 far 평면(z = 0)이 모서리를 지나게 한다.
// 실패(InvalidArgument): projection 비유한·역행렬 없음, 평면 비유한·영 법선, 카메라가 평면 위/양수
// 쪽, 평면이 반대쪽 frustum 모서리보다 멀어 보이는 영역이 없음(far 평면 너머 평면), depth와
// 행렬의 깊이 계수 부호 불일치(Standard는 M[2][2] > 0, Reversed는 <= 0).
// 실패(Unsupported): 직교 등 w가 view 위치에 의존하지 않는 투영(이 방법으로는 일부 평면에서 잘못
// 자른다).
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
    const T zero = static_cast<T>(0);
    // clip w = p · (M[*][3])다. view 위치 계수가 모두 0이면 w가 상수다(직교 투영).
    if (projection.At(0, 3) == zero && projection.At(1, 3) == zero && projection.At(2, 3) == zero) {
        return Result<Matrix4<T>>::Failure(ErrorCode::Unsupported);
    }
    // 원근 투영의 clip z는 view 깊이(z)에 대해 Standard면 증가(M[2][2] > 0), Reversed면 감소하거나
    // 무한 원거리에서 상수(M[2][2] <= 0)다. 잘못된 depth를 넘기면 보이는 쪽이 뒤집힌 행렬이 되므로
    // 거부한다.
    const bool reversed = depth == DepthRange::Reversed;
    if (reversed ? !(projection.At(2, 2) <= zero) : !(projection.At(2, 2) > zero)) {
        return Result<Matrix4<T>>::Failure(ErrorCode::InvalidArgument);
    }
    auto inverse = projection.TryInverse();
    if (!inverse) {
        return Result<Matrix4<T>>::Failure(inverse.Error());
    }
    // 행 벡터 규약에서 clip = p · M이므로 평면은 c' = c · (M⁻¹)ᵀ로, clip 점은 q = clip · M⁻¹로
    // view 공간에 돌아간다. 반대쪽 frustum 모서리 clip 점은 (sgn(c'.x), sgn(c'.y), far z, 1)이고
    // far z는 Standard에서 1, Reversed에서 0이다.
    const Vector4<T> clipSpacePlane = clipPlane * inverse.Value().Transposed();
    const Vector4<T> corner(math::Sign(clipSpacePlane.x), math::Sign(clipSpacePlane.y),
                            reversed ? static_cast<T>(0) : static_cast<T>(1), static_cast<T>(1));
    const Vector4<T> q = corner * inverse.Value();
    // 반대쪽 모서리가 평면의 보이는 쪽(양수)에 있어야 새 frustum이 비지 않는다. 평면이 far 평면
    // 너머에 있으면 음수가 되어 보이는 쪽이 뒤집힌 행렬이 나오므로 거부한다.
    const T denominator = Dot(clipPlane, q);
    if (!(denominator > zero)) {
        return Result<Matrix4<T>>::Failure(ErrorCode::InvalidArgument);
    }
    // a = (wColumn · q) / (c · q).
    // Standard: 새 z 열 = a * c (near z = 0이 평면 c, far z = w가 q를 지난다).
    // Reversed: 새 z 열 = wColumn - a * c (near w - z = a * c, far z = 0이 q를 지난다).
    const Vector4<T> wColumn = projection.Column(3);
    const Vector4<T> scaled = clipPlane * (Dot(wColumn, q) / denominator);
    const Vector4<T> zColumn = reversed ? wColumn - scaled : scaled;
    Matrix4<T> result = projection;
    result.At(0, 2) = zColumn.x;
    result.At(1, 2) = zColumn.y;
    result.At(2, 2) = zColumn.z;
    result.At(3, 2) = zColumn.w;
    return detail::FiniteOrInvalid(result);
}

// ---- view ----

// 카메라 회전에서 view 회전 행렬을 만든다(UE FInverseRotationMatrix(Rotation) * swizzle —
// LocalPlayer.cpp:1241-1245, SceneView.cpp:819-823). 카메라 로컬 전방(+X)·오른쪽(+Y)·위(+Z)가 view
// +Z·+X·+Y로 간다. 이동은 없다. 단위 quaternion 전제다.
// 결과는 UE의 Rotator 삼각함수 곱(InverseRotationMatrix.h:35-51) 대신 quaternion 회전 행렬의
// 전치로 만들므로 원소가 반올림 수준에서 다를 수 있다.
template <typename T>
Matrix4<T> MakeViewRotation(const Quaternion<T>& cameraRotation) noexcept {
    return cameraRotation.ToMatrix().Transposed() * detail::ViewAxisSwizzle<T>();
}

template <typename T>
Matrix4<T> MakeViewRotation(const Rotator<T>& cameraRotation) noexcept {
    return MakeViewRotation(cameraRotation.ToQuaternion());
}

// world → view 행렬 T(-viewOrigin) * MakeViewRotation(cameraRotation)이다(UE WorldToView,
// SceneView.cpp:684). 큰 월드에서는 카메라 상대 좌표(viewOrigin을 뺀 위치)로 행렬을 만들어 float
// 정밀도를 지킨다(UE PreViewTranslation·TranslatedViewMatrix와 같은 방식 — SceneView.cpp:696-707).
//
// 사용 예:
//   const Rotator<double> facingRight(0.0, 90.0, 0.0); // 카메라가 +Y(오른쪽)를 본다
//   const Mat4d worldToView = MakeWorldToView(Vec3d(0.0, 0.0, 170.0), facingRight);
//   // 월드 (0, 500, 170)은 view (0, 0, 500)이다.
template <typename T>
Matrix4<T> MakeWorldToView(const Vector3<T>& viewOrigin,
                           const Quaternion<T>& cameraRotation) noexcept {
    return Matrix4<T>::Translation(-viewOrigin) * MakeViewRotation(cameraRotation);
}

template <typename T>
Matrix4<T> MakeWorldToView(const Vector3<T>& viewOrigin,
                           const Rotator<T>& cameraRotation) noexcept {
    return MakeWorldToView(viewOrigin, cameraRotation.ToQuaternion());
}

// LookFrom이 up과 lookDirection 사이 각의 sin으로 요구하는 최솟값이다. 두 단위 벡터의 외적은
// 반올림으로 약 수 ULP의 길이를 가지므로 정확히 0인지로는 평행을 판정할 수 없다. 64 *
// MachineEpsilon은 float에서 약 7.6e-6 rad(0.0004도), double에서 약 1.4e-14 rad이다.
template <typename T>
    requires math::IsFloatingPointV<T>
inline constexpr T LookFromMinimumSine = static_cast<T>(64) * math::MachineEpsilon<T>;

// eye에서 lookDirection 방향을 보는 world → view 행렬이다(UE FLookFromMatrix — Matrix.inl:
// 957-976). view 축: Z = lookDirection(전방), X = up × Z(오른쪽), Y = Z × X(위)이고 M[*][0..2] 열이
// 각 축, 이동 행은 -eye · 축이다. up은 대략적인 위 방향이다.
// UE와 다른 점: UE는 평행 판정이 없다(GetSafeNormal이 0 벡터를 내 퇴화 행렬이 된다). Rocks는 up과
// lookDirection이 (거의) 평행하면(sin < LookFromMinimumSine) 실패하고, 기준 근처에서도 직교
// 행렬이 되도록 X축을 Z에 대해 Gram-Schmidt로 한 번 더 직교화한다(일반 입력에서는 UE와 반올림
// 수준에서 같다).
// 실패(InvalidArgument): 비유한 eye, 영·비유한 방향, 평행한 up.
template <typename T>
Result<Matrix4<T>> TryMakeLookFrom(const Vector3<T>& eye, const Vector3<T>& lookDirection,
                                   const Vector3<T>& up) noexcept {
    auto axisZ = lookDirection.TryNormalize();
    auto unitUp = up.TryNormalize();
    if (!eye.IsFinite() || !axisZ || !unitUp) {
        return Result<Matrix4<T>>::Failure(ErrorCode::InvalidArgument);
    }
    // 두 단위 벡터의 외적 길이는 sin이다.
    auto roughX = Cross(unitUp.Value(), axisZ.Value()).TryNormalize(LookFromMinimumSine<T>);
    if (!roughX) {
        return Result<Matrix4<T>>::Failure(ErrorCode::InvalidArgument);
    }
    // 거의 평행한 입력에서 외적의 반올림 오차(약 eps / sin)가 Z 방향 성분으로 남으므로 Gram-Schmidt
    // 한 번으로 Z에 직교화한다.
    auto axisX =
        (roughX.Value() - axisZ.Value() * Dot(axisZ.Value(), roughX.Value())).TryNormalize();
    if (!axisX) {
        return Result<Matrix4<T>>::Failure(ErrorCode::InvalidArgument);
    }
    const Vector3<T> x = axisX.Value();
    const Vector3<T> z = axisZ.Value();
    const Vector3<T> y = Cross(z, x);
    const Vector3<T> negativeEye = -eye;
    const T zero = static_cast<T>(0);
    const Matrix4<T> result =
        Matrix4<T>::FromRows(Vector4<T>(x.x, y.x, z.x, zero), Vector4<T>(x.y, y.y, z.y, zero),
                             Vector4<T>(x.z, y.z, z.z, zero),
                             Vector4<T>(Dot(negativeEye, x), Dot(negativeEye, y),
                                        Dot(negativeEye, z), static_cast<T>(1)));
    return detail::FiniteOrInvalid(result);
}

// eye에서 target을 보는 world → view 행렬이다(UE FLookAtMatrix). eye == target이면 실패한다.
template <typename T>
Result<Matrix4<T>> TryMakeLookAt(const Vector3<T>& eye, const Vector3<T>& target,
                                 const Vector3<T>& up) noexcept {
    return TryMakeLookFrom(eye, target - eye, up);
}

// ---- 기본 affine ----

// 단위 quaternion 회전 행렬(UE FQuatRotationMatrix)과 그 역(전치)이다. 역 회전은 UE
// FInverseRotationMatrix와 같은 회전이다(UE는 Rotator 삼각함수로 만든다).
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

// 회전 → 이동 행렬 R * T다(UE FQuatRotationTranslationMatrix(Q, Origin)).
template <typename T>
Matrix4<T> MakeRotationTranslation(const Quaternion<T>& rotation,
                                   const Vector3<T>& translation) noexcept {
    return MakeRotationTranslationMatrix(rotation, translation);
}

// scale → 회전 → 이동 행렬 S * R * T다(UE FScaleRotationTranslationMatrix(Scale, Rot, Origin)와
// 같은 인자 순서·의미, UE FTransform::ToMatrixWithScale과 같은 원소 —
// TransformVectorized.h:303-345). 회전 행렬의 행 0·1·2에 scale.x·y·z를 곱하고 마지막 행이 이동이다.
template <typename T>
Matrix4<T> MakeScaleRotationTranslation(const Vector3<T>& scale, const Quaternion<T>& rotation,
                                        const Vector3<T>& translation) noexcept {
    Matrix4<T> result = rotation.ToMatrix();
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

// pivot을 중심으로 회전하는 행렬 T(-pivot) * R * T(pivot)이다(UE FRotationAboutPointMatrix(Rot,
// Origin)).
template <typename T>
Matrix4<T> MakeRotationAboutPoint(const Quaternion<T>& rotation, const Vector3<T>& pivot) noexcept {
    const Vector3<T> offset = pivot - rotation.RotateVector(pivot);
    return MakeRotationTranslationMatrix(rotation, offset);
}

// Dot(normal, p) = distance 평면에 대한 반사 행렬이다(UE FMirrorMatrix(FPlane) — MirrorMatrix.h:
// 36-46과 원소가 같다). 3x3 부분은 대칭 I - 2nnᵀ, 이동 행은 2dn이다. 행렬식은 -1이므로 삼각형
// winding이 뒤집힌다. UE와 달리 법선을 정규화한다(UE는 단위 법선을 가정한다).
// 실패(InvalidArgument): 영·비유한 법선, 비유한 distance.
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
        result.At(3, row) = static_cast<T>(2) * distance * components[row];
    }
    return detail::FiniteOrInvalid(result);
}

} // namespace rocks
