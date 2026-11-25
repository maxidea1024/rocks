#pragma once

#include <UiWidget/WidgetSlot.h>
#include <UiFramework/Grid.h>

namespace rocks::ui {

// 행과 열에 자식을 배치한다. 슬롯은 셀 범위와 여백, 같은 위치에서의 그리기 순서를 보존한다. 배치는
// 위젯 트리의 슬롯에 저장되어 화면을 재구성할 때도 유지된다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API GridWidgetSlot final : public WidgetSlot {
    ROCKS_GENERATED_BODY()
public:
    Result<void> SetRow(ObjectRegistry& objects, uint32 value) noexcept;
    Result<void> SetColumn(ObjectRegistry& objects, uint32 value) noexcept;
    Result<void> SetRowSpan(ObjectRegistry& objects, uint32 value) noexcept;
    Result<void> SetColumnSpan(ObjectRegistry& objects, uint32 value) noexcept;
    Result<void> SetLayer(ObjectRegistry& objects, int32 value) noexcept;
    Result<void> SetNudge(ObjectRegistry& objects, Vec2 value) noexcept;
    Result<void> SetPadding(ObjectRegistry& objects, Vec4 value) noexcept;
    Result<void> SetHorizontalAlignment(ObjectRegistry& objects, StackAlignment value) noexcept;
    Result<void> SetVerticalAlignment(ObjectRegistry& objects, StackAlignment value) noexcept;

    GridSlot Layout() const noexcept;

    // 슬롯 핸들과 자식 포커스를 유지하여 배치를 바꾼다. 실패하면 기존 배치를 보존한다.
    Result<void> UpdateLayout(ObjectRegistry& objects, const GridSlot& layout) noexcept;

private:
    friend class GridPanelWidget;
    Result<void> Configure(const GridSlot& layout) noexcept;
    ROCKS_PROPERTY(ReadOnly, Name = "column") uint32 column_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "row") uint32 row_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "columnSpan") uint32 columnSpan_ = 1;
    ROCKS_PROPERTY(ReadOnly, Name = "rowSpan") uint32 rowSpan_ = 1;
    ROCKS_PROPERTY(ReadOnly, Name = "layer") int32 layer_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "padding") Vec4 padding_ {};
    ROCKS_PROPERTY(ReadOnly, Name = "horizontal") uint8 horizontal_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "vertical") uint8 vertical_ = 0;
    ROCKS_PROPERTY(ReadOnly, Name = "nudge") Vec2 nudge_ {};
};

} // namespace rocks::ui
