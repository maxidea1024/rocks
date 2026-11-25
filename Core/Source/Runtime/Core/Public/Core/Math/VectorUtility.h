#pragma once

#include <Core/Math/MathUtility.h>
#include <Core/Math/Quaternion.h>
#include <Core/Math/Rotator.h>
#include <Core/Math/Vector3.h>

// 이동·카메라가 쓰는 벡터 보조 함수다(UE FVector·FMath 대응 — Docs/Design/EngineRuntime 26.2절).
// 좌표·회전 규약은 UE 5.8과 같다(왼손 +X 전방·+Z 위, 각은 도).
namespace rocks::math {

// ClampedToMaxSize가 0을 돌려주는 최대 길이 하한이다(UE UE_KINDA_SMALL_NUMBER).
inline constexpr double MinClampedVectorSize = 1.0e-4;

// 길이가 maxSize보다 길면 그 길이로 줄인다(UE GetClampedToMaxSize — Vector.h:1904). maxSize <
// MinClampedVectorSize면 0 벡터다.
template <typename T>
Vector3<T> ClampedToMaxSize(const Vector3<T>& vector, T maxSize) noexcept {
    if (maxSize < static_cast<T>(MinClampedVectorSize)) {
        return Vector3<T>();
    }

    const T lengthSquared = vector.LengthSquared();
    if (lengthSquared > maxSize * maxSize) {
        return vector * (maxSize / Sqrt(lengthSquared));
    }

    return vector;
}

// unitNormal 평면 위로 투영한다: v − (v·n)n(UE FVector::VectorPlaneProject — Vector.h:1416). 법선은
// 단위 벡터여야 한다(0이면 v 그대로).
template <typename T>
constexpr Vector3<T> VectorPlaneProject(const Vector3<T>& vector,
                                        const Vector3<T>& unitNormal) noexcept {
    return vector - unitNormal * Dot(vector, unitNormal);
}

// 점을 (origin, unitNormal) 평면 위로 투영한다: p − ((p − origin)·n)n(UE FVector::PointPlaneProject
// — Vector.h:1408).
template <typename T>
constexpr Vector3<T> PointPlaneProject(const Vector3<T>& point, const Vector3<T>& origin,
                                       const Vector3<T>& unitNormal) noexcept {
    return point - unitNormal * Dot(point - origin, unitNormal);
}

// 방향을 바라보는 회전(roll 0)이다(UE FVector::ToOrientationRotator — UnrealMath.cpp:200): yaw =
// atan2(y, x), pitch = atan2(z, √(x² + y²))(도). 0 벡터는 (0, 0, 0)이다.
template <typename T>
Rotator<T> ToOrientationRotator(const Vector3<T>& direction) noexcept {
    const T yaw = RadiansToDegrees(Atan2(direction.y, direction.x));
    const T pitch = RadiansToDegrees(
        Atan2(direction.z, Sqrt(direction.x * direction.x + direction.y * direction.y)));
    return Rotator<T>(pitch, yaw, static_cast<T>(0));
}

// 같은 회전의 quaternion이다(UE FVector::ToOrientationQuat — UnrealMath.cpp:285): 반각 p·y로
// (sin p sin y, −sin p cos y, cos p sin y, cos p cos y).
template <typename T>
Quaternion<T> ToOrientationQuat(const Vector3<T>& direction) noexcept {
    const T yaw = Atan2(direction.y, direction.x);
    const T pitch = Atan2(direction.z, Sqrt(direction.x * direction.x + direction.y * direction.y));
    const T halfPitch = pitch * static_cast<T>(0.5);
    const T halfYaw = yaw * static_cast<T>(0.5);
    const T sinPitch = Sin(halfPitch);
    const T cosPitch = Cos(halfPitch);
    const T sinYaw = Sin(halfYaw);
    const T cosYaw = Cos(halfYaw);
    return Quaternion<T>(sinPitch * sinYaw, -sinPitch * cosYaw, cosPitch * sinYaw,
                         cosPitch * cosYaw);
}

} // namespace rocks::math
