#pragma once

#include <Core/Delegate.h>
#include <Core/Text/StringView.h>
#include <UiCore/VectorImageKey.h>

namespace rocks::ui {

// 메뉴 항목의 체크 표시 종류다(UE EUserInterfaceActionType ToggleButton·RadioButton).
enum class MenuCheckKind : uint8 {
    None,
    Check,
    Radio
};

// 메뉴 항목 하나의 동작이다(UE FUIAction + 항목 표시 정보 — Docs/Design/UiToolkit 9.1절 UW7c).
// 체크·라디오 항목은 isChecked로 표시를 정한다. icon은 아이콘 칸(체크 표시가 없는 항목)에 그리고,
// toolTip은 항목에 머무르면 보이는 설명이다(UE 명령 설명 툴팁). 둘 다 비워도 된다. toolTip
// 글자는 항목을 만들 때 복사한다.
struct MenuAction {
    Delegate<void()> execute{};
    Delegate<bool()> canExecute{};
    Delegate<bool()> isChecked{};
    MenuCheckKind check = MenuCheckKind::None;
    VectorImageKey icon{};
    StringView toolTip{};
};

} // namespace rocks::ui
