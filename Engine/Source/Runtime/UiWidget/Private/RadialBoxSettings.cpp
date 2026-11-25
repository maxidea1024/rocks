#include <UiWidget/RadialBoxSettings.h>

namespace rocks::ui {

bool RadialBoxSettings::IsValid() const noexcept {
    return math::IsFinite(startingAngle) && math::IsFinite(sectorAngle) && sectorAngle >= 0 &&
           sectorAngle <= 360 && math::IsFinite(angleBetweenItems) && angleBetweenItems >= 0 &&
           angleBetweenItems <= 360 && math::IsFinite(margin) && margin >= 0;
}
} // namespace rocks::ui
