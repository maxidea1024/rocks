#pragma once

#include <UiFramework/Layout/WrapStack.h>
#include <UiWidget/AlignedWidgetSlot.h>

namespace rocks::ui {

// 줄바꿈 패널의 자식 하나에 적용하는 배치 규칙이다. 부모가 다시 생성되어도 새 줄 시작,
// 남는 공간 채우기, 좁은 패널에서 한 줄 전체 차지와 여백·정렬을 그대로 복원한다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API WrapBoxWidgetSlot final : public AlignedWidgetSlot {
    ROCKS_GENERATED_BODY()
public:
    Result<void> SetNewLine(ObjectRegistry& objects, bool value) noexcept;
    Result<void> SetFillEmptySpace(ObjectRegistry& objects, bool value) noexcept;
    Result<void> SetFillSpanWhenLessThan(ObjectRegistry& objects, float value) noexcept;

    WrapSlot WrapLayout() const noexcept;

    // 슬롯 핸들과 자식 포커스를 유지하여 배치를 바꾼다. 실패하면 기존 배치를 보존한다.
    Result<void> UpdateLayout(ObjectRegistry& objects, const WrapSlot& layout) noexcept;

private:
    friend class WrapBoxWidget;
    Result<void> Configure(const WrapSlot& layout) noexcept;

    ROCKS_PROPERTY(ReadOnly, Name = "forceNewLine") bool forceNewLine_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "fillEmptySpace") bool fillEmptySpace_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "fillLineWhenSizeLessThan") float fillLineWhenSizeLessThan_ = 0;
};

} // namespace rocks::ui
