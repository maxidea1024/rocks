#pragma once
#include <Base/Guid.h>
#include <Meta/Annotations.h>

namespace rocks::ui {

// 방향별 규칙과 같은 WidgetTree의 안정 위젯 ID를 에셋에 기록한다.
ROCKS_STRUCT()
struct WidgetNavigationRule {
    ROCKS_GENERATED_BODY()
    ROCKS_PROPERTY() uint8 rule = 0;
    ROCKS_PROPERTY() Guid target {};
};
} // namespace rocks::ui
