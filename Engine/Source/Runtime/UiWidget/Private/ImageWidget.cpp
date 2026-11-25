#include "MakeWidgetElement.h"
#include <UiWidget/ImageWidget.h>
#include <UiFramework/Image.h>

namespace rocks::ui {

Result<SharedPtr<Element>> ImageWidget::BuildElement(ObjectRegistry& objects) noexcept {
    auto& allocator = objects.ObjectAllocator();
    return MakeWidgetElement<Image>(allocator);
}

Result<void> ImageWidget::SetImageSize(Vec2 value) noexcept {
    if (!value.IsFinite() || value.x < 0 || value.y < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    imageSize_ = value;
    if (auto live = CachedElement()) {
        ApplyProperties(*live);
    }

    return Result<void>::Success();
}

Result<void> ImageWidget::SetTint(LinearColor tint) noexcept {
    if (!tint.IsFinite()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    tint_ = tint;
    if (auto live = CachedElement()) {
        ApplyProperties(*live);
    }

    return Result<void>::Success();
}

Vec2 ImageWidget::ImageSize() const noexcept {
    return imageSize_;
}

Brush ImageWidget::GetBrush() const noexcept {
    auto result = brush_;
    result.imageSize = imageSize_;
    result.tint = tint_;
    return result;
}

LinearColor ImageWidget::Tint() const noexcept {
    return tint_;
}

void ImageWidget::ApplyProperties(Element& element) noexcept {
    auto& native = static_cast<Image&>(element);
    (void)native.SetBrush(GetBrush());
}

Result<void> ImageWidget::SetBrush(const Brush& brush) noexcept {
    if (!brush.IsValid()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    brush_ = brush;
    tint_ = brush.tint;
    return SetImageSize(brush.imageSize);
}

} // namespace rocks::ui
