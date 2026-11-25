#include <UiCore/Styles/StyleBrush.h>

namespace rocks::ui {

StyleBrush StyleBrush::None() noexcept {
    return {};
}

StyleBrush StyleBrush::RoundedBox(ThemeSlot fill, float radius, ThemeSlot outline,
                                  float outlineWidth) noexcept {
    StyleBrush brush;
    brush.mode = BrushMode::RoundedBox;
    brush.fill = ColorRef::FromTheme(fill);
    brush.outlineColor = ColorRef::FromTheme(outline);
    brush.outlineWidth = outlineWidth;
    brush.cornerRadii = {radius, radius, radius, radius};
    return brush;
}

StyleBrush StyleBrush::RoundedBox(ThemeSlot fill, float radius) noexcept {
    StyleBrush brush = RoundedBox(fill, radius, ThemeSlot::Black, 0.0f);
    brush.outlineColor = ColorRef::Specified({0, 0, 0, 0});
    return brush;
}

StyleBrush StyleBrush::Color(ThemeSlot fill) noexcept {
    StyleBrush brush;
    brush.mode = BrushMode::Box;
    brush.fill = ColorRef::FromTheme(fill);
    return brush;
}

Brush StyleBrush::Resolve(const ThemeColors& theme,
                          const StyleColorContext& context) const noexcept {
    Brush brush;
    brush.mode = mode;
    brush.tint = fill.Resolve(theme, context);
    brush.resource = resource;
    brush.imageSize = imageSize;
    brush.margin = margin;
    brush.tint.a *= opacity;
    brush.outline = {cornerRadii, outlineColor.Resolve(theme, context), outlineWidth, rounding};
    return brush;
}

} // namespace rocks::ui
