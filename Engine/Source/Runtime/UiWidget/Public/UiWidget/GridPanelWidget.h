#pragma once

#include <UiWidget/PanelWidget.h>
#include <UiFramework/Layout/Grid.h>

namespace rocks::ui {

// 행과 열에 자식을 배치한다. 슬롯은 셀 범위와 여백, 같은 위치에서의 그리기 순서를 보존한다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API GridPanelWidget final : public PanelWidget {
    ROCKS_GENERATED_BODY()
public:
    explicit GridPanelWidget(Allocator& allocator) noexcept;

    // 행·열의 가중치는 애셋에 보존된다. 0이면 콘텐츠 크기를 쓰며 ClearFill은 두 축을 초기화한다.
    Result<void> SetColumnFill(uint32 column, float weight) noexcept;
    Result<void> SetRowFill(uint32 row, float weight) noexcept;
    float ColumnFill(uint32 column) const noexcept;
    float RowFill(uint32 row) const noexcept;
    void ClearFill() noexcept;

    // 같은 WidgetTree의 자식을 지정한 셀에 붙이고 새 슬롯 핸들을 반환한다.
    Result<ObjectHandle> AddAt(ObjectRegistry& objects, ObjectHandle child,
                               const GridSlot& layout) noexcept;

protected:
    TypeId SlotType() const noexcept override;
    Result<SharedPtr<Element>> CreatePanel(Allocator& allocator) noexcept override;
    Result<void> AttachChild(ObjectRegistry& objects, ObjectHandle slot, Element& panel,
                             const SharedPtr<Element>& child) noexcept override;

private:
    Result<void> SetFill(Array<float>& weights, uint32 index, float value, bool column) noexcept;
    ROCKS_PROPERTY(ReadOnly, Name = "columnFill") Array<float> columnFill_;
    ROCKS_PROPERTY(ReadOnly, Name = "rowFill") Array<float> rowFill_;
};

} // namespace rocks::ui
