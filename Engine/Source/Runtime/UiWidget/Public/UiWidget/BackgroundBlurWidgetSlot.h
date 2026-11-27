#pragma once
#include <UiWidget/AlignedWidgetSlot.h>

namespace rocks::ui {

// BackgroundBlurWidget 자식의 배치 정보다. AddAlignedChild로 설정한 여백과 정렬은 위젯 애셋에
// 보존된다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API BackgroundBlurWidgetSlot final : public AlignedWidgetSlot {
    ROCKS_GENERATED_BODY()
};
} // namespace rocks::ui
