#pragma once

#include <Core/Guid.h>
#include <Meta/Annotations.h>

namespace rocks::ui {

// 컨트롤러 참조 필드와 위젯 또는 애니메이션 정의를 안정 ID로 연결한다.
// widget은 대상 ID다. 목적지 ObjectRef의 형식으로 위젯과 애니메이션을 구분한다.
ROCKS_STRUCT()
struct WidgetBinding {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() Guid field {};
    ROCKS_PROPERTY() Guid widget {};
    ROCKS_PROPERTY() bool optional = false;
};

} // namespace rocks::ui
