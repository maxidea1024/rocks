#pragma once

#include <Meta/Annotations.h>
#include <UiFramework/Controls/SelectionCause.h>

namespace rocks::ui {

// 선택이 바뀐 까닭이다(UE ESelectInfo). Widget 이벤트·속성용 리플렉션
// enum이다(Docs/Design/UiToolkit 21.5a절 — UE UMG ESlateVisibility처럼 Element 층 형식에 대응하는
// Widget 층 형식). Element 층 SelectionCause와 값 번호가 같다.
ROCKS_ENUM()
enum class WidgetSelectionCause : uint8 {
    Key = 0,
    Navigation = 1,
    Pointer = 2,
    Direct = 3,
};

static_assert(static_cast<uint8>(WidgetSelectionCause::Key) ==
                  static_cast<uint8>(SelectionCause::Key),
              "WidgetSelectionCause::Key must match SelectionCause::Key");
static_assert(static_cast<uint8>(WidgetSelectionCause::Navigation) ==
                  static_cast<uint8>(SelectionCause::Navigation),
              "WidgetSelectionCause::Navigation must match SelectionCause::Navigation");
static_assert(static_cast<uint8>(WidgetSelectionCause::Pointer) ==
                  static_cast<uint8>(SelectionCause::Pointer),
              "WidgetSelectionCause::Pointer must match SelectionCause::Pointer");
static_assert(static_cast<uint8>(WidgetSelectionCause::Direct) ==
                  static_cast<uint8>(SelectionCause::Direct),
              "WidgetSelectionCause::Direct must match SelectionCause::Direct");
static_assert(static_cast<uint8>(SelectionCause::Count) == 4,
              "SelectionCause gained or lost a value; update WidgetSelectionCause");

// Element 층 값과 바꾼다.
constexpr WidgetSelectionCause ToWidgetSelectionCause(SelectionCause value) noexcept {
    return static_cast<WidgetSelectionCause>(value);
}

constexpr SelectionCause ToElement(WidgetSelectionCause value) noexcept {
    return static_cast<SelectionCause>(value);
}

} // namespace rocks::ui
