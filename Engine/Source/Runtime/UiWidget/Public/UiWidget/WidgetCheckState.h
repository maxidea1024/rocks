#pragma once

#include <Meta/Annotations.h>
#include <UiFramework/Controls/CheckBox.h>

namespace rocks::ui {

// 체크 상자 상태다(UE ECheckBoxState). Widget 이벤트·속성용 리플렉션
// enum이다(Docs/Design/UiToolkit 21.5a절 — UE UMG ESlateVisibility처럼 Element 층 형식에 대응하는
// Widget 층 형식). Element 층 CheckState와 값 번호가 같다.
ROCKS_ENUM()
enum class WidgetCheckState : uint8 {
    Unchecked = 0,
    Checked = 1,
    Indeterminate = 2,
};

static_assert(static_cast<uint8>(WidgetCheckState::Unchecked) ==
                  static_cast<uint8>(CheckState::Unchecked),
              "WidgetCheckState::Unchecked must match CheckState::Unchecked");
static_assert(static_cast<uint8>(WidgetCheckState::Checked) ==
                  static_cast<uint8>(CheckState::Checked),
              "WidgetCheckState::Checked must match CheckState::Checked");
static_assert(static_cast<uint8>(WidgetCheckState::Indeterminate) ==
                  static_cast<uint8>(CheckState::Indeterminate),
              "WidgetCheckState::Indeterminate must match CheckState::Indeterminate");

// Element 층 값과 바꾼다.
constexpr WidgetCheckState ToWidgetCheckState(CheckState value) noexcept {
    return static_cast<WidgetCheckState>(value);
}

constexpr CheckState ToElement(WidgetCheckState value) noexcept {
    return static_cast<CheckState>(value);
}

} // namespace rocks::ui
