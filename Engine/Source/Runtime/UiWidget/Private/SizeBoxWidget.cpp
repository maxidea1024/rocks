#include <UiWidget/SizeBoxWidgetSlot.generated.h>
#include "MakeWidgetElement.h"
#include <UiWidget/SizeBoxWidget.h>
#include <UiFramework/Layout/SizeBox.h>

namespace rocks::ui {

TypeId SizeBoxWidget::SlotType() const noexcept {
    return GeneratedType<SizeBoxWidgetSlot>::Id();
}

SizeBoxWidget::SizeBoxWidget(Allocator& allocator) noexcept : ContentWidget(allocator) {
}

Result<SharedPtr<Element>> SizeBoxWidget::CreatePanel(Allocator& allocator) noexcept {
    return MakeWidgetElement<SizeBox>(allocator);
}

Result<void> SizeBoxWidget::SetMinimumSize(Vec2 value) noexcept {
    if (!value.IsFinite() || value.x < 0 || value.y < 0 || value.x > maximumSize_.x ||
        value.y > maximumSize_.y) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    minimumSize_ = value;
    if (auto live = CachedElement()) {
        ApplyProperties(*live);
    }

    return Result<void>::Success();
}

Result<void> SizeBoxWidget::SetMaximumSize(Vec2 value) noexcept {
    if (!value.IsFinite() || value.x < minimumSize_.x || value.y < minimumSize_.y) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    maximumSize_ = value;
    if (auto live = CachedElement()) {
        ApplyProperties(*live);
    }

    return Result<void>::Success();
}

Result<void> SizeBoxWidget::SetWidthOverride(float value) noexcept {
    if (!math::IsFinite(value) || value < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    widthOverride_ = value;
    if (auto live = CachedElement()) {
        return StaticElementCast<SizeBox>(*live).SetWidthOverride(value);
    }

    return Result<void>::Success();
}

void SizeBoxWidget::ClearWidthOverride() noexcept {
    widthOverride_ = -1;
    if (auto live = CachedElement()) {
        StaticElementCast<SizeBox>(*live).ClearWidthOverride();
    }
}

Result<void> SizeBoxWidget::SetHeightOverride(float value) noexcept {
    if (!math::IsFinite(value) || value < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    heightOverride_ = value;
    if (auto live = CachedElement()) {
        return StaticElementCast<SizeBox>(*live).SetHeightOverride(value);
    }

    return Result<void>::Success();
}

void SizeBoxWidget::ClearHeightOverride() noexcept {
    heightOverride_ = -1;
    if (auto live = CachedElement()) {
        StaticElementCast<SizeBox>(*live).ClearHeightOverride();
    }
}

Result<void> SizeBoxWidget::SetAspectRatioRange(float minimum, float maximum) noexcept {
    if (!SizeBox::IsValidAspectRange(minimum, maximum)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    aspectRange_ = {minimum, maximum};
    if (auto live = CachedElement()) {
        return StaticElementCast<SizeBox>(*live).SetAspectRatioRange(minimum, maximum);
    }

    return Result<void>::Success();
}

Vec2 SizeBoxWidget::MinimumSize() const noexcept {
    return minimumSize_;
}

void SizeBoxWidget::ApplyProperties(Element& element) noexcept {
    auto& native = StaticElementCast<SizeBox>(element);
    (void)native.SetSizeRange(minimumSize_, maximumSize_);
    (void)native.SetAspectRatioRange(aspectRange_.x, aspectRange_.y);
    if (widthOverride_ >= 0) {
        (void)native.SetWidthOverride(widthOverride_);
    } else {
        native.ClearWidthOverride();
    }

    if (heightOverride_ >= 0) {
        (void)native.SetHeightOverride(heightOverride_);
    } else {
        native.ClearHeightOverride();
    }
}

} // namespace rocks::ui
