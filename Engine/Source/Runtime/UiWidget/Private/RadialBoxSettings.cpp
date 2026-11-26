#include <UiWidget/RadialBoxSettings.h>

namespace rocks::ui {

namespace {

// 각도 설정 상한(한 바퀴)이다.
constexpr float RadialSettingsFullTurn = 360;

} // namespace

bool RadialBoxSettings::IsValid() const noexcept {
    return math::IsFinite(startingAngle) && math::IsFinite(sectorAngle) && sectorAngle >= 0 &&
           sectorAngle <= RadialSettingsFullTurn && math::IsFinite(angleBetweenItems) &&
           angleBetweenItems >= 0 && angleBetweenItems <= RadialSettingsFullTurn &&
           math::IsFinite(margin) && margin >= 0;
}
} // namespace rocks::ui
