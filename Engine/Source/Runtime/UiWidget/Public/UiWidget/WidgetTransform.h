#pragma once

#include <Meta/Annotations.h>
#include <UiCore/Geometry.h>

namespace rocks::ui {

// 로컬 피벗을 기준으로 배율, 기울기, 회전, 이동을 차례로 적용한다.
// 각도는 도 단위이고, 화면 좌표의 양의 회전 방향은 시계 방향이다.
ROCKS_STRUCT()
struct ROCKS_UIWIDGET_API WidgetTransform {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() Vec2 translation {};
    ROCKS_PROPERTY() Vec2 scale { 1, 1 };
    ROCKS_PROPERTY() Vec2 shear {};
    ROCKS_PROPERTY() float angle = 0;

    Result<AffineTransform2D> ToAffine() const noexcept;
};
} // namespace rocks::ui
