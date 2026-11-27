#pragma once

#include <Meta/Annotations.h>
#include <UiFramework/Controls/SpinBox.h>

namespace rocks::ui {

// 숫자 입력을 확정한 까닭이다. Widget 이벤트·속성용 리플렉션 enum이다(Docs/Design/UiToolkit 21.5a절
// — UE UMG ESlateVisibility처럼 Element 층 형식에 대응하는 Widget 층 형식). Element 층
// SpinCommitReason와 값 번호가 같다.
ROCKS_ENUM()
enum class WidgetSpinCommitReason : uint8 {
    Enter = 0,
    FocusLost = 1,
};

static_assert(static_cast<uint8>(WidgetSpinCommitReason::Enter) ==
                  static_cast<uint8>(SpinCommitReason::Enter),
              "WidgetSpinCommitReason::Enter must match SpinCommitReason::Enter");
static_assert(static_cast<uint8>(WidgetSpinCommitReason::FocusLost) ==
                  static_cast<uint8>(SpinCommitReason::FocusLost),
              "WidgetSpinCommitReason::FocusLost must match SpinCommitReason::FocusLost");

static_assert(static_cast<uint8>(SpinCommitReason::Count) == 2,
              "SpinCommitReason gained or lost a value; update WidgetSpinCommitReason");

// Element 층 값과 바꾼다.
constexpr WidgetSpinCommitReason ToWidgetSpinCommitReason(SpinCommitReason value) noexcept {
    return static_cast<WidgetSpinCommitReason>(value);
}

constexpr SpinCommitReason ToElement(WidgetSpinCommitReason value) noexcept {
    return static_cast<SpinCommitReason>(value);
}

} // namespace rocks::ui
