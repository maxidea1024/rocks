#include "GamepadMotorMapping.h"

#include <Core/Math/MathUtility.h>

namespace rocks::platform::gamepad {

namespace {

// [0, 1]로 자른 값의 65535 배를 반올림한다(NaN은 0).
uint16 MotorSpeed(float value) noexcept {
    const float clamped = value > 0.0f ? (value < 1.0f ? value : 1.0f) : 0.0f;
    return static_cast<uint16>(math::Round(clamped * static_cast<float>(MaxXInputMotorSpeed)));
}

float Larger(float a, float b) noexcept {
    return a > b ? a : b;
}

} // namespace

XInputMotorPair XInputMotorSpeeds(const input::MotorValues& values) noexcept {
    // UE XInputInterface.cpp:378-381: 왼쪽 = 큰 모터 둘의 최댓값, 오른쪽 = 작은 모터 둘의 최댓값.
    XInputMotorPair pair;
    pair.left = MotorSpeed(Larger(values.leftLarge, values.rightLarge));
    pair.right = MotorSpeed(Larger(values.leftSmall, values.rightSmall));
    return pair;
}

bool MotorValuesAreFinite(const input::MotorValues& values) noexcept {
    return math::IsFinite(values.leftLarge) && math::IsFinite(values.leftSmall) &&
           math::IsFinite(values.rightLarge) && math::IsFinite(values.rightSmall);
}

} // namespace rocks::platform::gamepad
