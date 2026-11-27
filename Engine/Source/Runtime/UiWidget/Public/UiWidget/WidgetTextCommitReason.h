#pragma once

#include <Meta/Annotations.h>
#include <UiFramework/Controls/TextCommitReason.h>

namespace rocks::ui {

// 편집 글자를 확정한 까닭이다(UE ETextCommit OnEnter·OnUserMovedFocus). Widget 이벤트·속성용
// 리플렉션 enum이다(Docs/Design/UiToolkit 21.5a절 — UE UMG ESlateVisibility처럼 Element 층 형식에
// 대응하는 Widget 층 형식). Element 층 TextCommitReason와 값 번호가 같다.
ROCKS_ENUM()
enum class WidgetTextCommitReason : uint8 {
    Enter = 0,
    FocusLost = 1,
};

static_assert(static_cast<uint8>(WidgetTextCommitReason::Enter) ==
                  static_cast<uint8>(TextCommitReason::Enter),
              "WidgetTextCommitReason::Enter must match TextCommitReason::Enter");
static_assert(static_cast<uint8>(WidgetTextCommitReason::FocusLost) ==
                  static_cast<uint8>(TextCommitReason::FocusLost),
              "WidgetTextCommitReason::FocusLost must match TextCommitReason::FocusLost");

// Element 층 값과 바꾼다.
constexpr WidgetTextCommitReason ToWidgetTextCommitReason(TextCommitReason value) noexcept {
    return static_cast<WidgetTextCommitReason>(value);
}

constexpr TextCommitReason ToElement(WidgetTextCommitReason value) noexcept {
    return static_cast<TextCommitReason>(value);
}

} // namespace rocks::ui
