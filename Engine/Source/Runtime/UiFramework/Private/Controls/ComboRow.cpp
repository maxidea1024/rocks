#include "ComboRow.h"

#include <UiFramework/Controls/ComboBox.h>

namespace rocks::ui {

ComboRow::ComboRow(Allocator& allocator, const ComboBox& owner, int32 index) noexcept
    : Button(allocator), owner_(owner), index_(index) {
    // 행은 Tab 정지가 아니다 — 콤보가 방향키로 강조를 옮긴다(UE SComboBox 목록).
    SetFocusable(false);
}

bool ComboRow::IsHighlighted() const noexcept {
    return Button::IsHighlighted() || owner_.HighlightedIndex() == index_;
}

} // namespace rocks::ui
