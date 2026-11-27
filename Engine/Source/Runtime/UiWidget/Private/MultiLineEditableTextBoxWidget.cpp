#include <UiWidget/MultiLineEditableTextBoxWidget.h>

namespace rocks::ui {

MultiLineEditableTextBoxWidget::MultiLineEditableTextBoxWidget(Allocator& allocator) noexcept
    : MultiLineEditableTextWidget(allocator) {
}

bool MultiLineEditableTextBoxWidget::HasBox() const noexcept {
    return true;
}

} // namespace rocks::ui
