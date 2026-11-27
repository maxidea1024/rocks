#include <UiFramework/Layout/HorizontalStack.h>

namespace rocks::ui {

HorizontalStack::HorizontalStack(Allocator& allocator) noexcept
    : StackPanel(allocator, StackDirection::Horizontal) {
}

} // namespace rocks::ui
