#include <UiFramework/MenuHeading.h>

namespace rocks::ui {

MenuHeading::MenuHeading(Allocator& allocator, const MenuStyle& style) noexcept
    : Element(allocator), style_(style) {
    SetVisibility(Visibility::HitTestInvisible);
}

Result<void> MenuHeading::SetLabel(const SharedPtr<TextLabel>& label) noexcept {
    if (!label) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (label_) {
        (void)RemoveChild(*label_);
    }

    label_ = label;
    label_->SetColor(style_.heading.color);
    return TryAddChild(label_);
}

Vec2 MenuHeading::ComputeDesiredSize(float) const noexcept {
    const Vec4 pad = style_.headingPadding;
    const Vec2 label = label_ ? label_->GetDesiredSize() : Vec2{};
    return {label.x + pad.x + pad.z, label.y + pad.y + pad.w};
}

Result<void> MenuHeading::Arrange(const Geometry& geometry) noexcept {
    SetGeometry(geometry);
    if (!label_) {
        return Result<void>::Success();
    }

    const Vec4 pad = style_.headingPadding;
    const Vec2 size{math::Max(0.0f, geometry.size.x - pad.x - pad.z),
                    math::Max(0.0f, geometry.size.y - pad.y - pad.w)};
    return label_->ArrangeIfNeeded(
        GetGeometry().MakeChild(size, {1, 0, 0, 1, pad.x, pad.y}, {}, {}));
}

} // namespace rocks::ui
