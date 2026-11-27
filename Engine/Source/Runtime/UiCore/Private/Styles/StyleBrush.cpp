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

StyleBrush StyleBrush::RoundedBox(LinearColor fill, float radius, LinearColor outline,
                                  float outlineWidth) noexcept {
    StyleBrush brush;
    brush.mode = BrushMode::RoundedBox;
    brush.fill = ColorRef::Specified(fill);
    brush.outlineColor = ColorRef::Specified(outline);
    brush.outlineWidth = outlineWidth;
    brush.cornerRadii = {radius, radius, radius, radius};
    return brush;
}

StyleBrush StyleBrush::Color(LinearColor fill) noexcept {
    StyleBrush brush;
    brush.mode = BrushMode::Box;
    brush.fill = ColorRef::Specified(fill);
    return brush;
}

StyleBrush StyleBrush::Pill(ThemeSlot fill) noexcept {
    StyleBrush brush = RoundedBox(fill, 0.0f);
    brush.rounding = BrushRounding::HalfHeight;
    return brush;
}

StyleBrush StyleBrush::Svg(VectorImageKey image, Vec2 size, ThemeSlot tint) noexcept {
    StyleBrush brush;
    brush.mode = BrushMode::Image;
    brush.fill = ColorRef::FromTheme(tint);
    brush.vectorImage = image;
    brush.imageSize = size;
    return brush;
}

StyleBrush StyleBrush::SvgForeground(VectorImageKey image, Vec2 size) noexcept {
    StyleBrush brush = Svg(image, size, ThemeSlot::Foreground);
    brush.fill = ColorRef{ColorSource::Foreground};
    return brush;
}

Brush StyleBrush::Resolve(const ThemeColors& theme,
                          const StyleColorContext& context) const noexcept {
    Brush brush;
    brush.mode = mode;
    brush.tint = fill.Resolve(theme, context);
    brush.resource = resource;
    brush.vectorImage = vectorImage;
    brush.imageSize = imageSize;
    brush.margin = margin;
    brush.tint.a *= opacity;
    brush.outline = {cornerRadii, outlineColor.Resolve(theme, context), outlineWidth, rounding};
    return brush;
}

} // namespace rocks::ui
