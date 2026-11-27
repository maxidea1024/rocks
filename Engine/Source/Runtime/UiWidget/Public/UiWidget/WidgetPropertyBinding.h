#pragma once

#include <Core/Guid.h>
#include <Meta/Annotations.h>

namespace rocks::ui {

// ViewModel의 필드 하나를 지정한 위젯의 쓰기 접근자에 연결한다.
// 한번만 적용하는 연결은 화면 생성 후 구독을 만들지 않는다.
ROCKS_STRUCT()
struct WidgetPropertyBinding {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() Guid sourceField {};
    ROCKS_PROPERTY() Guid widget {};
    ROCKS_PROPERTY() Guid targetField {};
    ROCKS_PROPERTY() bool once = false;
    // 최초 모델 값을 적용한 뒤 사용자가 대상 위젯을 바꾸면 원천 필드도 갱신한다.
    ROCKS_PROPERTY() bool twoWay = false;
};

} // namespace rocks::ui
