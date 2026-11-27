#pragma once

#include <UiCore/Geometry.h>
#include <Meta/Annotations.h>

namespace rocks::ui {

// 원호의 각도는 도 단위다. 균등 분배에서는 부채꼴을 항목 수로 나누며,
// 전체 원보다 작은 부채꼴은 각 구간의 중앙에 항목을 배치한다.
ROCKS_STRUCT()
struct ROCKS_UIWIDGET_API RadialBoxSettings {
    ROCKS_GENERATED_BODY()
    ROCKS_PROPERTY() float startingAngle = 0;
    ROCKS_PROPERTY() float sectorAngle = 360;
    ROCKS_PROPERTY() float angleBetweenItems = 0;
    ROCKS_PROPERTY() float margin = 0;
    ROCKS_PROPERTY() bool distributeEvenly = true;
    ROCKS_PROPERTY() bool clockwise = false;

    bool IsValid() const noexcept;
};
} // namespace rocks::ui
