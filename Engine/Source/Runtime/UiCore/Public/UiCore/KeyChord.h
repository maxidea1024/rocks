#pragma once

#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <Input/InputTypes.h>

namespace rocks::ui {

// 눌린 키와 당시의 보조 키 상태를 한 값으로 전달한다. Application이 사용자별로 추적하므로
// 포커스를 받기 전부터 누르고 있던 보조 키도 미리보기 단계에서 확인할 수 있다.
struct ROCKS_UICORE_API KeyChord {
    input::Key key = input::Key::None;
    bool shift = false;
    bool control = false;
    bool alt = false;
    bool super = false;
    input::MouseButton mouse = input::MouseButton::Count;
    input::GamepadButton gamepad = input::GamepadButton::Count;

    // 장치 버튼은 한 번에 하나만 설정한다. 설정 파일에는 장치와 버튼을 구분하는 이름을 쓴다.
    bool IsValid() const noexcept;
    const char* InputName() const noexcept;
    static Result<KeyChord> FromInputName(StringView name) noexcept;
    bool operator==(const KeyChord&) const noexcept = default;
};

} // namespace rocks::ui
