#pragma once

#include <UiWidget/PanelWidget.h>
#include <UiCore/StackLayout.h>
#include <Core/Math/IntVector.h>

namespace rocks::ui {

// 모든 셀을 같은 크기로 배치한다. AddAt의 셀 좌표는 x가 열, y가 행이며 0부터 시작한다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API UniformGridPanelWidget final : public PanelWidget {
    ROCKS_GENERATED_BODY()
public:
    explicit UniformGridPanelWidget(Allocator& allocator) noexcept;

    // 모든 셀에 같은 여백과 최소 크기를 적용한다. 부착 전에 설정해도 애셋에 보존된다.
    Result<void> SetCellPadding(Vec4 value) noexcept;
    Result<void> SetMinimumCellSize(Vec2 value) noexcept;
    Vec4 CellPadding() const noexcept;
    Vec2 MinimumCellSize() const noexcept;

    // 같은 WidgetTree에 만든 자식을 지정한 위치에 붙인다. 성공하면 해당 자식의 슬롯 핸들을
    // 반환한다.
    Result<ObjectHandle> AddAt(ObjectRegistry& objects, ObjectHandle child, const IntVec2& layout,
                               StackAlignment horizontal = StackAlignment::Fill,
                               StackAlignment vertical = StackAlignment::Fill) noexcept;

protected:
    TypeId SlotType() const noexcept override;
    Result<SharedPtr<Element>> CreatePanel(Allocator& allocator) noexcept override;
    Result<void> AttachChild(ObjectRegistry& objects, ObjectHandle slot, Element& panel,
                             const SharedPtr<Element>& child) noexcept override;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "cellPadding") Vec4 cellPadding_ {};
    ROCKS_PROPERTY(ReadOnly, Name = "minimumCellSize") Vec2 minimumCellSize_ {};
};

} // namespace rocks::ui
