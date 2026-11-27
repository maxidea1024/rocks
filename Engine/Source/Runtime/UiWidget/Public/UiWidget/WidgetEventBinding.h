#pragma once

#include <Core/Guid.h>
#include <Meta/Annotations.h>

namespace rocks::ui {

// 위젯 이름이나 C++ 함수 주소 대신 영속 ID를 저장한다. signature는 제작 시점의
// 인자 목록을 고정해 다른 버전의 모듈이 같은 이벤트에 다른 형식을 연결하지 못하게 한다.
ROCKS_STRUCT()
struct WidgetEventBinding {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() Guid widget {};
    ROCKS_PROPERTY() Guid eventId {};
    ROCKS_PROPERTY() Guid methodId {};
    ROCKS_PROPERTY() Guid signature {};
};

} // namespace rocks::ui
