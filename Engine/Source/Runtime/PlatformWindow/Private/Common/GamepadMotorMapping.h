#pragma once

#include <Base/Types.h>
#include <Input/MotorValues.h>

// 플레이어 모터 값(0–1 네 개)을 XInput 모터 두 개로 바꾸는 순수 함수다(Docs/Design/InputActions
// 6.4절 — UE XInputInterface.cpp:373-385). Win32 헤더 없이 값만 다뤄 모든 플랫폼에서 컴파일하고
// EngineTests로 시험한다. Win64 XInput backend가 SetMotors에서 쓴다.
namespace rocks::platform::gamepad {

// XINPUT_VIBRATION 모터 속도의 최대값이다(WORD).
inline constexpr uint32 MaxXInputMotorSpeed = 65535;

// XINPUT_VIBRATION의 두 값이다. left는 저주파(큰) 모터, right는 고주파(작은) 모터다.
struct XInputMotorPair {
    uint16 left = 0;
    uint16 right = 0;

    friend constexpr bool operator==(const XInputMotorPair&,
                                     const XInputMotorPair&) noexcept = default;
};

// 시험(GamepadMotorMappingTests)이 부르므로 export한다(Modular 링크).
// left = round(max(leftLarge, rightLarge)·65535), right = round(max(leftSmall,
// rightSmall)·65535)다. 값은 [0, 1]로 잘라 쓴다(비유한 값은 호출자가 거른다 — NaN은 0).
ROCKS_PLATFORMWINDOW_API XInputMotorPair
XInputMotorSpeeds(const input::MotorValues& values) noexcept;

// 네 값이 모두 유한한지다(SetMotors의 InvalidArgument 판정).
ROCKS_PLATFORMWINDOW_API bool MotorValuesAreFinite(const input::MotorValues& values) noexcept;

} // namespace rocks::platform::gamepad
