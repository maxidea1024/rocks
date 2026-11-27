#include <UiFramework/Layout/VerticalStack.h>

namespace rocks::ui {

VerticalStack::VerticalStack(Allocator& allocator) noexcept
    : StackPanel(allocator, StackDirection::Vertical) {
}

} // namespace rocks::ui
