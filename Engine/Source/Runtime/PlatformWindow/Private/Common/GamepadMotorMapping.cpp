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

// [0, 1]로 자른 값의 255 배를 버린다(NaN은 0 — UE ConvertToByte).
uint8 VibrationByte(float value) noexcept {
    const float clamped = value > 0.0f ? (value < 1.0f ? value : 1.0f) : 0.0f;
    return static_cast<uint8>(clamped * static_cast<float>(MaxAndroidVibrationIntensity));
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

AndroidVibratorPair AndroidVibratorIntensities(const input::MotorValues& values) noexcept {
    // UE AndroidInputInterface.cpp:629-630(Generic·Xbox): 왼쪽·오른쪽 각각 둘의 최댓값이다.
    AndroidVibratorPair pair;
    pair.left = VibrationByte(Larger(values.leftLarge, values.leftSmall));
    pair.right = VibrationByte(Larger(values.rightLarge, values.rightSmall));
    return pair;
}

bool NextAndroidVibration(AndroidVibrationState& state, const input::MotorValues& values,
                          bool focused, uint64 nowMilliseconds, AndroidVibratorPair& out) noexcept {
    const AndroidVibratorPair wanted =
        focused ? AndroidVibratorIntensities(values) : AndroidVibratorPair{};
    const bool active = !(state.written == AndroidVibratorPair{});
    // 시계가 뒤로 가지 않지만 0으로 시작한 상태와 겹쳐도 빼기가 넘치지 않게 본다.
    const uint64 elapsed = nowMilliseconds >= state.writtenAtMilliseconds
                               ? nowMilliseconds - state.writtenAtMilliseconds
                               : 0;
    if (active && elapsed >= AndroidVibrationPulseMilliseconds) {
        state.written = AndroidVibratorPair{};
    }

    if (state.written == wanted) {
        return false;
    }

    state.written = wanted;
    state.writtenAtMilliseconds = nowMilliseconds;
    out = wanted;
    return true;
}

} // namespace rocks::platform::gamepad
