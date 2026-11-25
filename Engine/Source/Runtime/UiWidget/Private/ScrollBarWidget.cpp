#include <UiWidget/ScrollBarWidget.h>
#include <UiWidget/ScrollBarWidget.generated.h>
#include <UiFramework/ScrollBar.h>

namespace rocks::ui {

Result<void> ScrollBarWidget::SetState(float offset, float thumbSize) noexcept {
    if (!math::IsFinite(offset) || !math::IsFinite(thumbSize) || thumbSize < 0 || thumbSize > 1) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    thumbSize_ = thumbSize;
    offset_ = math::Clamp(offset, 0.0f, 1.0f - thumbSize);
    if (auto live = CachedElement()) {
        return static_cast<ScrollBar&>(*live).SetState(offset_, thumbSize_);
    }
    return Result<void>::Success();
}

float ScrollBarWidget::Offset() const noexcept {
    return offset_;
}

float ScrollBarWidget::ThumbSize() const noexcept {
    return thumbSize_;
}

void ScrollBarWidget::SetHorizontal(bool horizontal) noexcept {
    horizontal_ = horizontal;
    if (auto live = CachedElement()) {
        static_cast<ScrollBar&>(*live).SetHorizontal(horizontal);
    }
}

void ScrollBarWidget::SetOnScrolled(Delegate<void(float)>&& callback) noexcept {
    scrolled_ = Move(callback);
}

Result<SharedPtr<Element>> ScrollBarWidget::BuildElement(ObjectRegistry& objects) noexcept {
    auto& allocator = objects.ObjectAllocator();
    auto made = SharedPtr<ScrollBar>::TryMake(allocator, allocator);
    if (!made) {
        return Result<SharedPtr<Element>>::Failure(made.Error());
    }
    auto& bar = *made.Value();
    auto initialized = bar.SetState(offset_, thumbSize_);
    if (!initialized) {
        return Result<SharedPtr<Element>>::Failure(initialized.Error());
    }
    bar.SetHorizontal(horizontal_);
    bar.SetOnScrolled(Delegate<void(float)>::CreateLambda(
        [&objects, owner = Handle(), native = &bar](float value) noexcept {
            auto widget = objects.ResolveForWrite<ScrollBarWidget>(objects.OwnerAccess(), owner);
            if (widget) {
                auto* current = widget.Value().Get().Value();
                if (current->CachedElement().Get() == native) {
                    current->offset_ = value;
                    current->scrolled_.ExecuteIfBound(value);
                }
            }
        }));
    return Result<SharedPtr<Element>>::Success(Move(made.Value()));
}

} // namespace rocks::ui
