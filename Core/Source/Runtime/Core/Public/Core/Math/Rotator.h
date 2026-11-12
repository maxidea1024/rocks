#pragma once

#include <Core/Math/Quaternion.h>

namespace rocks {

// 사람이 편집하기 쉬운 오일러 각 회전이다(UE FRotator 대응). 단위는 라디안이다.
// Rocks 축(+X 전방, +Y 왼쪽, +Z 위)에서 각 성분의 의미:
// - yaw: +Z 축 회전. 양수는 위에서 볼 때 반시계, 즉 전방이 왼쪽으로 돈다.
// - pitch: 오른쪽 축(-Y) 회전. 양수는 기수가 위로 든다.
// - roll: 전방 축(+X) 회전. 양수는 왼쪽이 위로 올라간다.
// 적용 순서는 roll → pitch → yaw(내재 Z-Y'-X'')이며 quaternion으로는 Yaw * Pitch * Roll이다.
// UE FRotator는 도(degree) 단위·왼손 좌표계(+Y 오른쪽)이고 yaw 양수가 오른쪽 회전이므로 값을
// 그대로 옮기지 않는다. 같은 회전에 대응하는 각도 조합은 여러 개이며 비교는 quaternion으로 한다.
template <typename T>
struct Rotator {
    static_assert(math::IsFloatingPointV<T>, "Rotator supports float and double");

    T yaw = static_cast<T>(0);
    T pitch = static_cast<T>(0);
    T roll = static_cast<T>(0);

    constexpr Rotator() noexcept = default;

    constexpr Rotator(T yawRadians, T pitchRadians, T rollRadians) noexcept
        : yaw(yawRadians), pitch(pitchRadians), roll(rollRadians) {
    }

    static constexpr Rotator FromDegrees(T yawDegrees, T pitchDegrees, T rollDegrees) noexcept {
        return Rotator(math::DegreesToRadians(yawDegrees), math::DegreesToRadians(pitchDegrees),
                       math::DegreesToRadians(rollDegrees));
    }

    friend constexpr bool operator==(const Rotator&, const Rotator&) noexcept = default;

    constexpr bool IsFinite() const noexcept {
        return math::IsFinite(yaw) && math::IsFinite(pitch) && math::IsFinite(roll);
    }

    // 각 성분을 (-pi, pi]로 감는다. 회전 자체는 바뀌지 않는다.
    Rotator Normalized() const noexcept {
        return Rotator(math::UnwindRadians(yaw), math::UnwindRadians(pitch),
                       math::UnwindRadians(roll));
    }

    // 단위 quaternion으로 변환한다. 비유한 각은 비유한 quaternion이 되므로 먼저 IsFinite로
    // 검사한다.
    Quaternion<T> ToQuaternion() const noexcept {
        const T half = static_cast<T>(0.5);
        const Quaternion<T> yawRotation(static_cast<T>(0), static_cast<T>(0), math::Sin(yaw * half),
                                        math::Cos(yaw * half));
        // 오른쪽 축(-Y) 회전 = +Y 축 음의 각 회전
        const Quaternion<T> pitchRotation(static_cast<T>(0), -math::Sin(pitch * half),
                                          static_cast<T>(0), math::Cos(pitch * half));
        const Quaternion<T> rollRotation(math::Sin(roll * half), static_cast<T>(0),
                                         static_cast<T>(0), math::Cos(roll * half));
        return yawRotation * pitchRotation * rollRotation;
    }

    // 단위 quaternion을 yaw ∈ (-pi, pi], pitch ∈ [-pi/2, pi/2], roll ∈ (-pi, pi]로 분해한다.
    // pitch가 ±90도에 가까우면(전방 축의 수평 성분 < gimbalTolerance) yaw와 roll이 같은 축을
    // 공유하므로 roll = 0으로 두고 yaw에 합친다. 이 경우에도 ToQuaternion 결과는 같은 회전이다.
    // 실패(InvalidArgument): 비유한 또는 정규화할 수 없는 quaternion.
    static Result<Rotator> TryFromQuaternion(const Quaternion<T>& rotation,
                                             T gimbalTolerance = static_cast<T>(1.0e-6)) noexcept {
        auto unit = rotation.TryNormalize();
        if (!unit) {
            return Result<Rotator>::Failure(ErrorCode::InvalidArgument);
        }
        const Quaternion<double> q(unit.Value());
        const Vector3<double> forward = q.RotateVector(Vector3<double>::Forward());
        const Vector3<double> left = q.RotateVector(Vector3<double>::Left());
        const double horizontal = math::Sqrt(forward.x * forward.x + forward.y * forward.y);
        const double pitchValue = math::Atan2(forward.z, horizontal);
        double yawValue = 0.0;
        double rollValue = 0.0;
        if (horizontal > static_cast<double>(gimbalTolerance)) {
            yawValue = math::Atan2(forward.y, forward.x);
            // roll이 없을 때의 왼쪽·위 축에 대한 실제 왼쪽 축의 각이 roll이다.
            const double sinYaw = math::Sin(yawValue);
            const double cosYaw = math::Cos(yawValue);
            const double sinPitch = math::Sin(pitchValue);
            const double cosPitch = math::Cos(pitchValue);
            const Vector3<double> leftNoRoll(-sinYaw, cosYaw, 0.0);
            const Vector3<double> upNoRoll(-sinPitch * cosYaw, -sinPitch * sinYaw, cosPitch);
            rollValue = math::Atan2(Dot(left, upNoRoll), Dot(left, leftNoRoll));
        } else {
            // roll = 0이면 왼쪽 축은 (-sin yaw, cos yaw, 0)이다.
            yawValue = math::Atan2(-left.x, left.y);
        }
        // atan2(-0, 음수)는 -pi이고 -pi 근처 double을 float으로 바꾸면 -Pi<float>가 될 수 있다. T로
        // 바꾼 뒤 문서의 반열린 구간 (-pi, pi]에 맞춰 +pi로 바꾼다.
        const auto halfOpen = [](double radians) noexcept {
            const T value = static_cast<T>(radians);
            return value <= -math::Pi<T> ? math::Pi<T> : value;
        };
        return Result<Rotator>::Success(
            Rotator(halfOpen(yawValue), static_cast<T>(pitchValue), halfOpen(rollValue)));
    }
};

} // namespace rocks
