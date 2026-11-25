#include <UiFramework/Frame.h>

namespace rocks::ui {

Result<void> Frame::Construct(Args&& args) noexcept {
    auto status = args.content.AttachTo(*this);
    if (status) {
        status = SetBrush(args.brush);
    }
    if (status) {
        status = SetPadding(args.padding);
    }
    if (status) {
        BindColor(Move(args.color));
        BindDesiredSize(Move(args.desiredSize));
    }
    return status;
}

Frame::Frame(Allocator& allocator) noexcept
    : Element(allocator), color_(*this, {1, 1, 1, 1}), desiredSize_(*this, {}) {
}

Result<void> Frame::TryAddChild(const SharedPtr<Element>& child) noexcept {
    if (ChildCount() != 0) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }
    return Element::TryAddChild(child);
}

Result<void> Frame::SetBrush(const Brush& brush) noexcept {
    if (!brush.IsValid()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    if (brush_ != brush) {
        brush_ = brush;
        InvalidatePaint();
    }
    return Result<void>::Success();
}

const Brush& Frame::GetBrush() const noexcept {
    return brush_;
}

void Frame::SetColor(LinearColor color) noexcept {
    color_.Set(color);
}

void Frame::BindColor(Bindable<LinearColor> source) noexcept {
    color_.Assign(Move(source));
}

LinearColor Frame::GetColor() const noexcept {
    return color_.Get();
}

void Frame::SetDesiredSize(Vec2 size) noexcept {
    desiredSize_.Set(size);
}

void Frame::BindDesiredSize(Bindable<Vec2> source) noexcept {
    desiredSize_.Assign(Move(source));
}

Result<void> Frame::SetPadding(Vec4 padding) noexcept {
    if (!padding.IsFinite() || padding.x < 0 || padding.y < 0 || padding.z < 0 || padding.w < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (padding_ != padding) {
        padding_ = padding;
        InvalidateLayout();
    }
    return Result<void>::Success();
}

Vec2 Frame::ComputeDesiredSize(float) const noexcept {
    const Element* child = ChildAt(0);
    const Vec2 childSize = child != nullptr && child->GetVisibility() != Visibility::Collapsed
                               ? child->GetDesiredSize()
                               : Vec2{};
    const Vec2 size(childSize.x + padding_.x + padding_.z, childSize.y + padding_.y + padding_.w);
    return Vec2(math::Max(size.x, desiredSize_.Get().x), math::Max(size.y, desiredSize_.Get().y));
}

Result<void> Frame::Arrange(const Geometry& geometry) noexcept {
    SetGeometry(geometry);
    Element* child = ChildAt(0);
    if (child != nullptr && child->GetVisibility() != Visibility::Collapsed) {
        const Vec2 size(math::Max(0.0f, geometry.size.x - padding_.x - padding_.z),
                        math::Max(0.0f, geometry.size.y - padding_.y - padding_.w));
        const AffineTransform2D offset{1, 0, 0, 1, padding_.x, padding_.y};
        return child->ArrangeIfNeeded(GetGeometry().MakeChild(size, offset, {}, {}));
    }
    return Result<void>::Success();
}

Result<void> Frame::OnPaint(PaintList& list, int32 layer) const noexcept {
    return list.TryAddBrush(GetGeometry(), brush_, color_.Get(), layer);
}

} // namespace rocks::ui
