#pragma once

#include <UiWidget/WidgetSlot.h>
#include <UiCore/StackLayout.h>
#include <Core/Math/IntVector.h>

namespace rocks::ui {

// 모든 셀을 같은 크기로 배치한다. AddAt의 셀 좌표는 x가 열, y가 행이며 0부터 시작한다. 배치는 위젯
// 트리의 슬롯에 저장되어 화면을 재구성할 때도 유지된다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API UniformGridWidgetSlot final : public WidgetSlot {
    ROCKS_GENERATED_BODY()
public:
    Result<void> SetColumn(ObjectRegistry& objects, int32 value) noexcept;
    Result<void> SetRow(ObjectRegistry& objects, int32 value) noexcept;
    Result<void> SetHorizontalAlignment(ObjectRegistry& objects, StackAlignment value) noexcept;
    Result<void> SetVerticalAlignment(ObjectRegistry& objects, StackAlignment value) noexcept;
    IntVec2 Layout() const noexcept;
    StackAlignment HorizontalAlignment() const noexcept;
    StackAlignment VerticalAlignment() const noexcept;

    // 슬롯 핸들과 자식 포커스를 유지하여 배치를 바꾼다. 실패하면 기존 배치를 보존한다.
    Result<void> UpdateLayout(ObjectRegistry& objects, const IntVec2& layout,
                              StackAlignment horizontal, StackAlignment vertical) noexcept;

private:
    friend class UniformGridPanelWidget;
    Result<void> Configure(const IntVec2& layout, StackAlignment horizontal,
                           StackAlignment vertical) noexcept;
    ROCKS_PROPERTY(ReadOnly, Name = "x") int32 x_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "y") int32 y_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "horizontal") uint8 horizontal_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "vertical") uint8 vertical_ = 0;
};

} // namespace rocks::ui
