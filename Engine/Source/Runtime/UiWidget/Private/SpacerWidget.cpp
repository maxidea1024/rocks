#include "MakeWidgetElement.h"
#include <UiWidget/SpacerWidget.h>
#include <UiFramework/Layout/SizeBox.h>

namespace rocks::ui {

Result<SharedPtr<Element>> SpacerWidget::BuildElement(ObjectRegistry& objects) noexcept {
    auto& allocator = objects.ObjectAllocator();
    return MakeWidgetElement<SizeBox>(allocator);
}

Result<void> SpacerWidget::SetSize(Vec2 value) noexcept {
    if (!value.IsFinite() || value.x < 0 || value.y < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    size_ = value;
    if (auto live = CachedElement()) {
        ApplyProperties(*live);
    }

    return Result<void>::Success();
}

Vec2 SpacerWidget::Size() const noexcept {
    return size_;
}

void SpacerWidget::ApplyProperties(Element& element) noexcept {
    auto& native = static_cast<SizeBox&>(element);
    (void)native.SetWidthOverride(size_.x);
    (void)native.SetHeightOverride(size_.y);
    native.SetVisibility(Visibility::HitTestInvisible);
}

} // namespace rocks::ui
