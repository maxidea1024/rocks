#include <UiFramework/Layout/FitBox.h>

namespace rocks::ui {

namespace {

double ConstrainScale(double scale, FitDirection direction) noexcept {
    if (direction == FitDirection::ShrinkOnly) {
        return math::Min(1.0, scale);
    }

    if (direction == FitDirection::GrowOnly) {
        return math::Max(1.0, scale);
    }

    return scale;
}

} // namespace

FitBox::FitBox(Allocator& allocator) noexcept : Element(allocator) {
}

Result<void> FitBox::TryAddChild(const SharedPtr<Element>& child) noexcept {
    if (ChildCount() != 0) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    return Element::TryAddChild(child);
}

void FitBox::SetFit(FitMode mode, FitDirection direction) noexcept {
    if (mode_ == mode && direction_ == direction) {
        return;
    }

    if (mode_ == FitMode::User || mode == FitMode::User) {
        InvalidateLayout();
    } else {
        InvalidateArrangement();
    }

    mode_ = mode;
    direction_ = direction;
}

Result<void> FitBox::SetUserScale(float scale) noexcept {
    if (!math::IsFinite(scale) || scale < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (userScale_ != scale) {
        userScale_ = scale;
        InvalidateLayout();
    }

    return Result<void>::Success();
}

Result<void> FitBox::SetAlignment(Vec2 alignment) noexcept {
    if (!alignment.IsFinite() || alignment.x < 0 || alignment.x > 1 || alignment.y < 0 ||
        alignment.y > 1) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (alignment_ != alignment) {
        alignment_ = alignment;
        InvalidateArrangement();
    }

    return Result<void>::Success();
}

Vec2 FitBox::ComputeDesiredSize(float) const noexcept {
    const Element* child = ChildAt(0);
    if (!child || child->GetVisibility() == Visibility::Collapsed) {
        return {};
    }

    return child->GetDesiredSize() *
           (mode_ == FitMode::User ? static_cast<float>(ConstrainScale(userScale_, direction_))
                                   : 1.0f);
}

Result<void> FitBox::Arrange(const Geometry& geometry) noexcept {
    if (!geometry.size.IsFinite() || geometry.size.x < 0 || geometry.size.y < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    SetGeometry(geometry);
    Element* child = ChildAt(0);
    if (!child || child->GetVisibility() == Visibility::Collapsed) {
        return Result<void>::Success();
    }

    // 지난 배율로 측정한 자식 크기를 다시 맞추면 프레임마다 배율이 흔들릴 수 있다.
    // 부모 측정 때 저장한 자연 크기를 배치의 기준으로 사용한다.
    const float parentScale = MeasuredLayoutScale();
    Vec2 local = parentScale > 0 ? GetDesiredSize() : child->GetDesiredSize();
    double scale = mode_ == FitMode::User ? ConstrainScale(userScale_, direction_) : 1;
    if (mode_ == FitMode::User && scale > 0 && parentScale > 0) {
        local = local / static_cast<float>(scale);
    }

    if (mode_ == FitMode::Fill) {
        local = geometry.size;
    } else if (local.x > 0 && local.y > 0) {
        const double x = static_cast<double>(geometry.size.x) / local.x;
        const double y = static_cast<double>(geometry.size.y) / local.y;
        switch (mode_) {
        case FitMode::Fit:
            scale = math::Min(x, y);
            break;
        case FitMode::FitWidth:
            scale = x;
            break;
        case FitMode::FitHeight:
            scale = y;
            break;
        case FitMode::Cover:
            scale = math::Max(x, y);
            break;
        default:
            break;
        }

        scale = ConstrainScale(scale, direction_);
    }

    const float factor = static_cast<float>(scale);
    const Vec2 offset = (geometry.size - local * factor) * alignment_;
    if (!math::IsFinite(factor) || !offset.IsFinite()) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    if (factor > 0 && parentScale > 0) {
        auto measured = child->Prepass(parentScale * factor);
        if (!measured) {
            return measured;
        }
    }

    const AffineTransform2D transform{factor, 0, 0, factor, offset.x, offset.y};
    return child->ArrangeIfNeeded(GetGeometry().MakeChild(local, transform, {}, {}));
}

} // namespace rocks::ui
