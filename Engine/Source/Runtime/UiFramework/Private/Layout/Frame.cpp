#include <UiFramework/Layout/Frame.h>

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
        SetContentAlignment(args.horizontalAlignment.ValueOr(horizontal_),
                            args.verticalAlignment.ValueOr(vertical_));
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
                               ? child->SlotDesiredSize()
                               : Vec2{};
    const Vec2 size(childSize.x + padding_.x + padding_.z, childSize.y + padding_.y + padding_.w);
    return Vec2(math::Max(size.x, desiredSize_.Get().x), math::Max(size.y, desiredSize_.Get().y));
}

Vec2 Frame::MeasureForWidth(float width) noexcept {
    Element* child = ChildAt(0);
    if (child == nullptr || child->GetVisibility() == Visibility::Collapsed) {
        return GetDesiredSize();
    }

    const float inner = math::Max(0.0f, width - padding_.x - padding_.z);
    const Vec2 measured = child->MeasureInSlot(inner);
    return {math::Max(measured.x + padding_.x + padding_.z, desiredSize_.Get().x),
            math::Max(measured.y + padding_.y + padding_.w, desiredSize_.Get().y)};
}

void Frame::SetContentAlignment(StackAlignment horizontal, StackAlignment vertical) noexcept {
    if (horizontal_ != horizontal || vertical_ != vertical) {
        horizontal_ = horizontal;
        vertical_ = vertical;
        InvalidateArrangement();
    }
}

namespace {

// 한 축에서 내용의 크기와 시작 위치를 정한다(Fill은 칸 전체).
void PlaceAxis(StackAlignment alignment, float area, float desired, float& size,
               float& start) noexcept {
    if (alignment == StackAlignment::Fill) {
        size = area;
        start = 0;
        return;
    }

    size = math::Min(area, desired);
    start = alignment == StackAlignment::Start    ? 0.0f
            : alignment == StackAlignment::Center ? (area - size) * 0.5f
                                                  : area - size;
}

} // namespace

Result<void> Frame::Arrange(const Geometry& geometry) noexcept {
    SetGeometry(geometry);
    Element* child = ChildAt(0);
    if (child != nullptr && child->GetVisibility() != Visibility::Collapsed) {
        const Vec2 area(math::Max(0.0f, geometry.size.x - padding_.x - padding_.z),
                        math::Max(0.0f, geometry.size.y - padding_.y - padding_.w));
        const Vec2 desired = child->SlotDesiredSize();
        Vec2 size;
        Vec2 start;
        PlaceAxis(horizontal_, area.x, desired.x, size.x, start.x);
        PlaceAxis(vertical_, area.y, desired.y, size.y, start.y);
        const AffineTransform2D offset{1, 0, 0, 1, padding_.x + start.x, padding_.y + start.y};
        return child->ArrangeInSlot(GetGeometry().MakeChild(size, offset, {}, {}));
    }

    return Result<void>::Success();
}

Result<void> Frame::OnPaint(PaintList& list, int32 layer) const noexcept {
    return list.TryAddBrush(GetGeometry(), brush_, color_.Get(), layer);
}

} // namespace rocks::ui
