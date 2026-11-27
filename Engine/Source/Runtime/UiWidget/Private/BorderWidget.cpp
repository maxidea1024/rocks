#include <UiWidget/BorderWidgetSlot.generated.h>
#include "MakeWidgetElement.h"
#include <UiWidget/BorderWidget.h>
#include <UiFramework/Layout/Frame.h>

namespace rocks::ui {

TypeId BorderWidget::SlotType() const noexcept {
    return GeneratedType<BorderWidgetSlot>::Id();
}

BorderWidget::BorderWidget(Allocator& allocator) noexcept : ContentWidget(allocator) {
}

Result<SharedPtr<Element>> BorderWidget::CreatePanel(Allocator& allocator) noexcept {
    return MakeWidgetElement<Frame>(allocator);
}

Result<void> BorderWidget::SetTint(LinearColor value) noexcept {
    if (!value.IsFinite()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    tint_ = value;
    if (auto live = CachedElement()) {
        ApplyProperties(*live);
    }

    return Result<void>::Success();
}

Result<void> BorderWidget::SetBrush(const Brush& brush) noexcept {
    if (!brush.IsValid()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    brush_ = brush;
    if (auto live = CachedElement()) {
        return StaticElementCast<Frame>(*live).SetBrush(brush);
    }

    return Result<void>::Success();
}

const Brush& BorderWidget::GetBrush() const noexcept {
    return brush_;
}

Result<void> BorderWidget::SetPadding(Vec4 padding) noexcept {
    if (!SlotPlacement{.padding = padding}.IsValid()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    padding_ = padding;
    if (auto live = CachedElement()) {
        return StaticElementCast<Frame>(*live).SetPadding(padding);
    }

    return Result<void>::Success();
}

Vec4 BorderWidget::Padding() const noexcept {
    return padding_;
}

LinearColor BorderWidget::Tint() const noexcept {
    return tint_;
}

void BorderWidget::ApplyProperties(Element& element) noexcept {
    auto& native = StaticElementCast<Frame>(element);
    native.SetColor(tint_);
    (void)native.SetBrush(brush_);
    (void)native.SetPadding(padding_);
}

} // namespace rocks::ui
