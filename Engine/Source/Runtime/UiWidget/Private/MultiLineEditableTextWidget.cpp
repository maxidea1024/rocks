#include <UiWidget/MultiLineEditableTextWidget.h>

namespace rocks::ui {

MultiLineEditableTextWidget::MultiLineEditableTextWidget(Allocator& allocator) noexcept
    : EditableTextWidget(allocator) {
    (void)SetAutoWrapText(true);
}

bool MultiLineEditableTextWidget::IsMultiline() const noexcept {
    return true;
}

} // namespace rocks::ui
