#pragma once

#include <Core/Result.h>

namespace rocks::config {
class ConfigContext;
} // namespace rocks::config

// input.* 설정 변수다(Docs/Design/TouchInput.ko.md 3.4·5.1절, Docs/Design/InputActions 6.3절).
// Input 모듈의 RegisterSettings가 등록하고, 설정 문맥이 묶인 게임 스레드(엔진 프레임 — 값은 sync
// 지점에서만 바뀐다)에서 읽는다.
// - input.use_mouse_for_touch(기본 false, UE bUseMouseForTouch): 마우스 왼쪽 버튼으로 손가락 0번
//   터치를 흉내 낸다.
// - input.enable_gesture_recognizer(기본 false, UE bEnableGestureRecognizer): PlayerController의
//   Pinch·Rotate·Flick 인식 기본값이다.
// - input.haptic_scale(기본 1, UE ForceFeedbackScale): 플레이어 진동 세기 배율이다(곱한 뒤 [0, 1]).
// - input.enable_haptics(기본 true, UE bForceFeedbackEnabled): 거짓이면 모터에 0을 쓴다.
namespace rocks::input {

// input.* 변수를 등록한다(모듈 RegisterSettings가 부르고, 설정 문맥을 직접 만드는 시험도 부른다).
// 같은 문맥에 다시 부르면 멱등이다.
ROCKS_INPUT_API Result<void> RegisterInputSettings(config::ConfigContext& context) noexcept;

// input.use_mouse_for_touch다. 호스트가 프레임마다 InputState::SetMouseTouchEmulation에 넣는다.
// 설정 문맥이 묶인 스레드에서만 부른다(아니면 Fatal — CVar::Get 계약).
ROCKS_INPUT_API bool UseMouseForTouchSetting() noexcept;

// input.enable_gesture_recognizer다. GameInstance가 프레임마다 PlayerController의 기본값으로
// 넣는다. 설정 문맥이 묶인 스레드에서만 부른다.
ROCKS_INPUT_API bool GestureRecognizerSetting() noexcept;

// input.haptic_scale이다. PlayerController가 틱마다 진동 합성에 넣는다. 설정 문맥이 묶인
// 스레드에서만 부른다.
ROCKS_INPUT_API float HapticScaleSetting() noexcept;

// input.enable_haptics다. PlayerController가 틱마다 진동 합성에 넣는다. 설정 문맥이 묶인
// 스레드에서만 부른다.
ROCKS_INPUT_API bool EnableHapticsSetting() noexcept;

} // namespace rocks::input
