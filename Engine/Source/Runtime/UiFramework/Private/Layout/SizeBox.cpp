#include <UiFramework/Layout/SizeBox.h>

namespace rocks::ui {

SizeBox::SizeBox(Allocator& allocator) noexcept : Element(allocator) {
}

Result<void> SizeBox::TryAddChild(const SharedPtr<Element>& child) noexcept {
    if (ChildCount() != 0) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    return Element::TryAddChild(child);
}

Result<void> SizeBox::SetSizeRange(Vec2 minimum, Vec2 maximum) noexcept {
    if (!minimum.IsFinite() || !maximum.IsFinite() || minimum.x < 0 || minimum.y < 0 ||
        maximum.x < minimum.x || maximum.y < minimum.y) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (minimum_ != minimum || maximum_ != maximum) {
        minimum_ = minimum;
        maximum_ = maximum;
        InvalidateLayout();
    }

    return Result<void>::Success();
}

Result<void> SizeBox::SetWidthOverride(float width) noexcept {
    if (!math::IsFinite(width) || width < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (!widthOverride_ || overrideSize_.x != width) {
        overrideSize_.x = width;
        widthOverride_ = true;
        InvalidateLayout();
    }

    return Result<void>::Success();
}

Result<void> SizeBox::SetHeightOverride(float height) noexcept {
    if (!math::IsFinite(height) || height < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (!heightOverride_ || overrideSize_.y != height) {
        overrideSize_.y = height;
        heightOverride_ = true;
        InvalidateLayout();
    }

    return Result<void>::Success();
}

void SizeBox::ClearWidthOverride() noexcept {
    if (widthOverride_) {
        widthOverride_ = false;
        InvalidateLayout();
    }
}

void SizeBox::ClearHeightOverride() noexcept {
    if (heightOverride_) {
        heightOverride_ = false;
        InvalidateLayout();
    }
}

bool SizeBox::IsValidAspectRange(float minimum, float maximum) noexcept {
    return math::IsFinite(minimum) && math::IsFinite(maximum) && minimum >= 0 && maximum >= 0 &&
           (maximum == 0 || minimum <= maximum);
}

Result<void> SizeBox::SetAspectRatioRange(float minimum, float maximum) noexcept {
    if (!IsValidAspectRange(minimum, maximum)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const Vec2 range{minimum, maximum};
    if (aspectRange_ != range) {
        aspectRange_ = range;
        InvalidateArrangement();
    }

    return Result<void>::Success();
}

Vec2 SizeBox::ComputeDesiredSize(float) const noexcept {
    const Element* child = ChildAt(0);
    const Vec2 size =
        child && child->GetVisibility() != Visibility::Collapsed ? child->GetDesiredSize() : Vec2{};
    return Vec2(math::Clamp(widthOverride_ ? overrideSize_.x : size.x, minimum_.x, maximum_.x),
                math::Clamp(heightOverride_ ? overrideSize_.y : size.y, minimum_.y, maximum_.y));
}

Result<void> SizeBox::Arrange(const Geometry& geometry) noexcept {
    SetGeometry(geometry);
    Element* child = ChildAt(0);
    if (child && child->GetVisibility() != Visibility::Collapsed) {
        Vec2 size = geometry.size;
        if (size.x > 0 && size.y > 0) {
            const double ratio = static_cast<double>(size.x) / size.y;
            if (aspectRange_.x > 0 && ratio < aspectRange_.x) {
                size.y = size.x / aspectRange_.x;
            } else if (aspectRange_.y > 0 && ratio > aspectRange_.y) {
                size.x = size.y * aspectRange_.y;
            }
        }

        const auto offset = (geometry.size - size) * 0.5f;
        return child->ArrangeIfNeeded(
            GetGeometry().MakeChild(size, {1, 0, 0, 1, offset.x, offset.y}, {}, {}));
    }

    return Result<void>::Success();
}

} // namespace rocks::ui
