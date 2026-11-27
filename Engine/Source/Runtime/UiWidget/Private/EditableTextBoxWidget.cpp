#include <UiWidget/EditableTextBoxWidget.h>

namespace rocks::ui {

EditableTextBoxWidget::EditableTextBoxWidget(Allocator& allocator) noexcept
    : EditableTextWidget(allocator) {
}

bool EditableTextBoxWidget::HasBox() const noexcept {
    return true;
}

} // namespace rocks::ui
