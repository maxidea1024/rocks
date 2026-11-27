#include <UiFramework/Lists/CarouselPage.h>

namespace rocks::ui {

CarouselPage::CarouselPage(Allocator& allocator, Factory&& factory) noexcept
    : SizeBox(allocator), factory_(Move(factory)) {
}

Result<void> CarouselPage::Materialize() noexcept {
    if (ChildCount()) {
        return Result<void>::Success();
    }

    if (!factory_) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    auto child = factory_.Execute();
    return child ? TryAddChild(child.Value()) : Result<void>::Failure(child.Error());
}

void CarouselPage::ReleaseContent() noexcept {
    if (auto child = ChildRefAt(0)) {
        (void)RemoveChild(*child);
    }
}

} // namespace rocks::ui
