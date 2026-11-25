#pragma once

namespace rocks::input {

// 게임패드 모터 네 개의 세기다(0–1 — UE FForceFeedbackValues, Docs/Design/InputActions 6.1절).
// 큰 모터는 저주파(묵직한 진동), 작은 모터는 고주파다. 플레이어의 진동 합성(InputActions
// HapticPlayer)이 만들고 호스트가 GamepadBackend::SetMotors로 패드에 쓴다.
//
// 사용 예:
//   input::MotorValues motors;
//   motors.leftLarge = 0.5f;
//   (void)gamepads->SetMotors(0, motors);
struct MotorValues {
    float leftLarge = 0.0f;
    float leftSmall = 0.0f;
    float rightLarge = 0.0f;
    float rightSmall = 0.0f;

    friend constexpr bool operator==(const MotorValues&, const MotorValues&) noexcept = default;
};

} // namespace rocks::input
