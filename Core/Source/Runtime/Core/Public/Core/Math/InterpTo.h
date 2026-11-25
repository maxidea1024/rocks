#pragma once

#include <Core/Math/InterpCurveMath.h>
#include <Core/Math/MathUtility.h>
#include <Core/Math/Quaternion.h>
#include <Core/Math/Vector3.h>

// 프레임 시간 기반 지수형 보간(UE FMath::VInterpTo·QInterpTo — UnrealMath.cpp:2584·2752)이다.
// 카메라 지연(SpringArm)·이동 컴포넌트가 쓴다(Docs/Design/EngineRuntime 26.2절).
namespace rocks::math {

// VInterpTo·QInterpTo가 "도착"으로 보는 허용 오차다(UE UE_KINDA_SMALL_NUMBER).
inline constexpr double InterpToArrivalTolerance = 1.0e-4;

// current에서 target 쪽으로 clamp(deltaSeconds·speed, 0, 1)만큼 옮긴다(UE VInterpTo). speed ≤
// 0이거나 거리² < InterpToArrivalTolerance면 target이다. 단위는 입력과 같다(위치면 cm).
//
// 사용 예(카메라 지연, speed 10):
//   cameraLocation = math::VInterpTo(cameraLocation, target, deltaSeconds, 10.0);
template <typename T>
Vector3<T> VInterpTo(const Vector3<T>& current, const Vector3<T>& target, T deltaSeconds,
                     T speed) noexcept {
    if (speed <= static_cast<T>(0)) {
        return target;
    }

    const Vector3<T> distance = target - current;
    if (distance.LengthSquared() < static_cast<T>(InterpToArrivalTolerance)) {
        return target;
    }

    return current + distance * Clamp(deltaSeconds * speed, static_cast<T>(0), static_cast<T>(1));
}

// current에서 target 쪽으로 clamp(speed·deltaSeconds, 0, 1)만큼 구면 보간한다(UE QInterpTo —
// FQuat::Slerp와 같은 CurveSlerp). speed ≤ 0이거나 두 회전이 성분 차 ≤ InterpToArrivalTolerance로
// 같으면(부호 반대 q·−q도 같다 — UE FQuat::Equals) target이다. 단위 quaternion 전제다.
template <typename T>
Quaternion<T> QInterpTo(const Quaternion<T>& current, const Quaternion<T>& target, T deltaSeconds,
                        T speed) noexcept {
    if (speed <= static_cast<T>(0)) {
        return target;
    }

    const T tolerance = static_cast<T>(InterpToArrivalTolerance);
    const bool same =
        Abs(current.x - target.x) <= tolerance && Abs(current.y - target.y) <= tolerance &&
        Abs(current.z - target.z) <= tolerance && Abs(current.w - target.w) <= tolerance;
    const bool opposite =
        Abs(current.x + target.x) <= tolerance && Abs(current.y + target.y) <= tolerance &&
        Abs(current.z + target.z) <= tolerance && Abs(current.w + target.w) <= tolerance;
    if (same || opposite) {
        return target;
    }

    return CurveSlerp(current, target,
                      Clamp(speed * deltaSeconds, static_cast<T>(0), static_cast<T>(1)));
}

} // namespace rocks::math
