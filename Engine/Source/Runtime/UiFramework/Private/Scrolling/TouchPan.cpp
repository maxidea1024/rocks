#include <UiCore/InputMetrics.h>
#include "TouchPan.h"

namespace rocks::ui {

bool TryTouchPan(const Geometry& geometry, bool vertical, Vec2 origin, Vec2 position, double offset,
                 double maximum, float& start, float& current) noexcept {
    AffineTransform2D inverse;
    if (!geometry.render.TryInverse(inverse)) {
        return false;
    }

    const Vec2 first = inverse.Apply(origin);
    const Vec2 last = inverse.Apply(position);
    if (!first.IsFinite() || !last.IsFinite()) {
        return false;
    }

    start = vertical ? first.y : first.x;
    current = vertical ? last.y : last.x;
    const float delta = start - current;
    const float cross = vertical ? last.x - first.x : last.y - first.y;
    return math::Abs(delta) > PointerDragThreshold && math::Abs(delta) >= math::Abs(cross) &&
           ((delta > 0 && offset < maximum) || (delta < 0 && offset > 0));
}
} // namespace rocks::ui
