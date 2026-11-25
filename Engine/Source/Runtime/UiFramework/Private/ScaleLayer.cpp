#include <UiFramework/ScaleLayer.h>

namespace rocks::ui {

ScaleLayer::ScaleLayer(Allocator& allocator) noexcept : Element(allocator) {
}

Result<void> ScaleLayer::TryAddChild(const SharedPtr<Element>& child) noexcept {
    if (ChildCount() != 0) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }
    return Element::TryAddChild(child);
}

Result<void> ScaleLayer::SetScale(float scale) noexcept {
    if (!math::IsFinite(scale) || scale <= 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (scale_ != scale) {
        scale_ = scale;
        InvalidateLayout();
    }
    return Result<void>::Success();
}

float ScaleLayer::RelativeLayoutScale() const noexcept {
    return scale_;
}

Vec2 ScaleLayer::ComputeDesiredSize(float) const noexcept {
    const Element* child = ChildAt(0);
    return child ? child->GetDesiredSize() * scale_ : Vec2{};
}

Result<void> ScaleLayer::Arrange(const Geometry& geometry) noexcept {
    SetGeometry(geometry);
    Element* child = ChildAt(0);
    if (!child || child->GetVisibility() == Visibility::Collapsed) {
        return Result<void>::Success();
    }

    const Vec2 size = geometry.size / scale_;
    if (!size.IsFinite() || size.x < 0 || size.y < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    const AffineTransform2D transform{scale_, 0, 0, scale_, 0, 0};
    return child->ArrangeIfNeeded(GetGeometry().MakeChild(size, transform, {}, {}));
}

} // namespace rocks::ui
