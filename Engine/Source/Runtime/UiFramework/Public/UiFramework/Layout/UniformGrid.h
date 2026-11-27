#pragma once

#include <Core/Math/Vector4.h>
#include <UiCore/SlotPlacement.h>

namespace rocks::ui {

// 모든 셀이 같은 크기를 갖는 행과 열 배치다.
class ROCKS_UIFRAMEWORK_API UniformGrid : public Element {
public:
    explicit UniformGrid(Allocator& allocator) noexcept;

    Result<void> TryAddChild(const SharedPtr<Element>& child) noexcept override;
    bool RemoveChild(Element& child) noexcept override;

    // 같은 셀에 여러 자식을 넣을 수 있으며 추가 순서대로 그린다.
    Result<void> TryAddSlot(const SharedPtr<Element>& child, uint32 row, uint32 column,
                            StackAlignment horizontal = StackAlignment::Fill,
                            StackAlignment vertical = StackAlignment::Fill) noexcept;
    Result<void> SetSlot(Element& child, uint32 row, uint32 column, StackAlignment horizontal,
                         StackAlignment vertical) noexcept;
    Result<void> SetCellPadding(Vec4 padding) noexcept;
    Result<void> SetMinimumCellSize(Vec2 size) noexcept;

    Vec2 ComputeDesiredSize(float scale) const noexcept override;
    Result<void> Arrange(const Geometry& geometry) noexcept override;

private:
    struct Slot {
        uint32 row;
        uint32 column;
        StackAlignment horizontal;
        StackAlignment vertical;
    };

    void GetDimensions(uint32& rows, uint32& columns) const noexcept;

    Array<Slot> slots_;
    Vec4 padding_{};
    Vec2 minimumCellSize_{};
};

} // namespace rocks::ui
