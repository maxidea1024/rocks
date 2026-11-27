#include <UiFramework/Controls/Image.h>

namespace rocks::ui {

Image::Image(Allocator& allocator) noexcept : Element(allocator) {
    brush_.mode = BrushMode::None;
    SetVisibility(Visibility::SelfHitTestInvisible);
}

Result<void> Image::SetBrush(const Brush& brush) noexcept {
    if (!brush.IsValid()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (brush_ == brush) {
        return Result<void>::Success();
    }

    const Vec2 previous = ComputeDesiredSize(1);
    brush_ = brush;
    if (previous != ComputeDesiredSize(1)) {
        InvalidateLayout();
    } else {
        InvalidatePaint();
    }

    return Result<void>::Success();
}

Result<void> Image::TryAddChild(const SharedPtr<Element>&) noexcept {
    return Result<void>::Failure(ErrorCode::Unsupported);
}

Vec2 Image::ComputeDesiredSize(float) const noexcept {
    return brush_.mode == BrushMode::None ? Vec2{} : brush_.imageSize;
}

Result<void> Image::OnPaint(PaintList& list, int32 layer) const noexcept {
    return list.TryAddBrush(GetGeometry(), brush_, LinearColor(1, 1, 1, 1), layer);
}

AccessibleRole Image::GetAccessibleRole() const noexcept {
    return AccessibleRole::Image;
}

} // namespace rocks::ui
