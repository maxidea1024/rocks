#include <UiCore/Brush.h>

#include <Core/Math/MathUtility.h>

namespace rocks::ui {

bool Brush::IsValid() const noexcept {
    const bool outlineValid = outline.cornerRadii.IsFinite() && outline.cornerRadii.x >= 0 &&
                              outline.cornerRadii.y >= 0 && outline.cornerRadii.z >= 0 &&
                              outline.cornerRadii.w >= 0 && outline.color.IsFinite() &&
                              math::IsFinite(outline.width) && outline.width >= 0;
    return (mode == BrushMode::None || mode == BrushMode::Image || mode == BrushMode::Box ||
            mode == BrushMode::Border || mode == BrushMode::RoundedBox) &&
           outlineValid && imageSize.IsFinite() && imageSize.x >= 0 && imageSize.y >= 0 &&
           uv.IsValid() && margin.IsFinite() && margin.x >= 0 && margin.y >= 0 && margin.z >= 0 &&
           margin.w >= 0 && margin.x + margin.z <= 1 && margin.y + margin.w <= 1 &&
           tint.IsFinite() && (vectorImage.IsNull() || mode == BrushMode::Image);
}

Brush Brush::RoundedBox(LinearColor fill, float radius, LinearColor outlineColor,
                        float outlineWidth) noexcept {
    return RoundedBox(fill, {radius, radius, radius, radius}, outlineColor, outlineWidth);
}

Brush Brush::RoundedBox(LinearColor fill, Vec4 radii, LinearColor outlineColor,
                        float outlineWidth) noexcept {
    Brush brush;
    brush.mode = BrushMode::RoundedBox;
    brush.tint = fill;
    brush.outline = {radii, outlineColor, outlineWidth, BrushRounding::Fixed};
    return brush;
}

Brush Brush::PillBox(LinearColor fill, LinearColor outlineColor, float outlineWidth) noexcept {
    Brush brush;
    brush.mode = BrushMode::RoundedBox;
    brush.tint = fill;
    brush.outline = {{}, outlineColor, outlineWidth, BrushRounding::HalfHeight};
    return brush;
}

Brush Brush::Vector(VectorImageKey image, Vec2 size, LinearColor tint) noexcept {
    Brush brush;
    brush.mode = BrushMode::Image;
    brush.vectorImage = image;
    brush.imageSize = size;
    brush.tint = tint;
    return brush;
}

} // namespace rocks::ui
