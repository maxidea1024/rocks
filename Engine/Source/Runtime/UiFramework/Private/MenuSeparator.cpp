#include <UiFramework/MenuSeparator.h>

namespace rocks::ui {

MenuSeparator::MenuSeparator(Allocator& allocator, const MenuStyle& style) noexcept
    : Element(allocator), style_(style) {
    SetVisibility(Visibility::HitTestInvisible);
}

Vec2 MenuSeparator::ComputeDesiredSize(float) const noexcept {
    const Vec4 pad = style_.separatorPadding;
    return {pad.x + pad.z, style_.separatorThickness + pad.y + pad.w};
}

Result<void> MenuSeparator::OnPaint(PaintList& list, int32 layer) const noexcept {
    const Vec4 pad = style_.separatorPadding;
    const Geometry& geometry = GetGeometry();
    const Vec2 size{math::Max(0.0f, geometry.size.x - pad.x - pad.z), style_.separatorThickness};
    const auto line = geometry.MakeChild(size, {1, 0, 0, 1, pad.x, pad.y}, {}, {});
    return list.TryAddBrush(line, style_.separator.Resolve(list.Theme(), list.ColorContext()),
                            LinearColor{1, 1, 1, 1}, layer);
}

} // namespace rocks::ui
