#pragma once

#include <Core/Optional.h>
#include <Input/InputEvent.h>
#include <UiCore/KeyChord.h>
#include <UiCore/NavigationRule.h>

namespace rocks::ui {

// 키가 뜻하는 UI 동작이다(UE EUINavigationAction — NavigationConfig.cpp 31–38행).
enum class NavigationAction : uint8 {
    None,
    Accept,
    Back
};

// Element가 받는 키 이벤트다(Docs/Design/UiToolkit 20.7절). Application이 사용자의
// NavigationConfig로 이 키의 navigation 방향과 동작을 미리 계산해 넣는다(Element는 Application을
// 모른다). input은 라우팅 동안만 유효하다.
struct KeyEvent {
    uint32 user = 0;
    const input::InputEvent& input;
    KeyChord chord{};
    Optional<FocusDirection> navigation{};
    NavigationAction action = NavigationAction::None;
};

} // namespace rocks::ui
