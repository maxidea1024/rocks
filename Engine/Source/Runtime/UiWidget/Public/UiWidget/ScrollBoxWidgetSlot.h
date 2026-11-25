#pragma once
#include <UiWidget/AlignedWidgetSlot.h>
#include <UiCore/StackLayout.h>

namespace rocks::ui {

// ScrollBoxWidget 자식의 배치 정보다. AddAlignedChild로 설정한 여백과 정렬은 위젯 애셋에 보존된다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API ScrollBoxWidgetSlot final : public AlignedWidgetSlot {
    ROCKS_GENERATED_BODY()
public:
    // Auto는 콘텐츠의 주축 측정값을 쓰며 Fill은 남은 뷰포트 공간을 가중치로 배분한다.
    Result<void> SetSize(ObjectRegistry& objects, StackSizeRule rule, float weight = 1) noexcept;
    StackSizeRule SizeRule() const noexcept;
    float FillWeight() const noexcept;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "sizeRule") uint8 sizeRule_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "fillWeight") float fillWeight_ = 1;
};
} // namespace rocks::ui
