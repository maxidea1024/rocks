#include <UiCore/Brush.h>

namespace rocks::ui {

bool Brush::IsValid() const noexcept {
    return (mode == BrushMode::None || mode == BrushMode::Image || mode == BrushMode::Box ||
            mode == BrushMode::Border) &&
           imageSize.IsFinite() && imageSize.x >= 0 && imageSize.y >= 0 && uv.IsValid() &&
           margin.IsFinite() && margin.x >= 0 && margin.y >= 0 && margin.z >= 0 && margin.w >= 0 &&
           margin.x + margin.z <= 1 && margin.y + margin.w <= 1 && tint.IsFinite();
}

} // namespace rocks::ui
