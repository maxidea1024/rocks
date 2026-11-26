#include <UiCore/Styles/CoreStyles.h>
#include <UiFramework/WindowAppIcon.h>

namespace rocks::ui {

WindowAppIcon::WindowAppIcon(Allocator& allocator, const WindowStyle& style) noexcept
    : Element(allocator), style_(style) {
}

input::WindowRegion WindowAppIcon::GetWindowRegion() const noexcept {
    return input::WindowRegion::SystemMenu;
}

Vec2 WindowAppIcon::ComputeDesiredSize(float) const noexcept {
    const Vec4 padding = style_.appIconPadding;
    return {style_.appIconSize.x + padding.x + padding.z,
            style_.appIconSize.y + padding.y + padding.w};
}

Result<void> WindowAppIcon::OnPaint(PaintList& list, int32 layer) const noexcept {
    const Vec4 padding = style_.appIconPadding;
    const auto icon =
        GetGeometry().MakeChild(style_.appIconSize, {1, 0, 0, 1, padding.x, padding.y}, {}, {});
    const LinearColor ink = list.ForegroundColor();
    return list.TryAddBrush(icon, Brush::Vector(style_.appIcon, style_.appIconSize), ink, layer);
}

} // namespace rocks::ui
