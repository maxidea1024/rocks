#pragma once

#include <Core/Guid.h>
#include <Curves/TangentCurve.h>
#include <Meta/Annotations.h>

namespace rocks::ui {

enum class WidgetAnimationChannel : uint8 {
    Opacity,
    TranslationX,
    TranslationY,
    ScaleX,
    ScaleY,
    ShearX,
    ShearY,
    Angle
};

// target은 위젯 트리의 안정 ID다. 빈 GUID는 재생을 요청한 UserWidget 자체를 뜻한다.
// 곡선 키의 시간은 초이며 선형·계단·가중 탄젠트 보간은 엔진 TangentCurve 규칙을 따른다.
ROCKS_STRUCT()
struct ROCKS_UIWIDGET_API WidgetAnimationTrack {
    ROCKS_GENERATED_BODY()
    explicit WidgetAnimationTrack(Allocator& allocator) noexcept : curve(allocator) {
    }
    ROCKS_PROPERTY() Guid target {};
    ROCKS_PROPERTY() uint8 channel = 0;
    ROCKS_PROPERTY() TangentCurve curve;
};

} // namespace rocks::ui
