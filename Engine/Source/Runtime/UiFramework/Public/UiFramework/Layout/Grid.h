#pragma once

#include <Core/Math/Vector4.h>
#include <UiCore/SlotPlacement.h>

namespace rocks::ui {

struct GridSlot {
    uint32 column = 0;
    uint32 row = 0;
    uint32 columnSpan = 1;
    uint32 rowSpan = 1;
    int32 layer = 0;
    Vec4 padding{};
    StackAlignment horizontal = StackAlignment::Fill;
    StackAlignment vertical = StackAlignment::Fill;
    // 측정 크기를 유지하면서 패널 로컬 좌표에서 내용 위치만 옮긴다.
    Vec2 nudge{};
};

// 자식이 여러 칸을 차지하면 원하는 크기를 해당 행과 열에 균등하게 나누어 반영한다.
class ROCKS_UIFRAMEWORK_API Grid : public Element {
    ROCKS_UI_ELEMENT(Grid, Element)

public:
    explicit Grid(Allocator& allocator) noexcept;

    Result<void> TryAddSlot(const SharedPtr<Element>& child, const GridSlot& slot) noexcept;
    // 기존 자식을 분리하지 않고 셀 위치·정렬·레이어를 갱신한다.
    Result<void> SetSlot(Element& child, const GridSlot& slot) noexcept;
    Result<void> TryAddChild(const SharedPtr<Element>& child) noexcept override;
    bool RemoveChild(Element& child) noexcept override;

    // 계수가 0인 행과 열은 측정 크기를 쓰고, 나머지는 남은 공간을 계수 비율로 나눈다.
    void ClearFill() noexcept;
    Result<void> SetColumnFill(uint32 column, float weight) noexcept;
    Result<void> SetRowFill(uint32 row, float weight) noexcept;

    Vec2 ComputeDesiredSize(float scale) const noexcept override;
    Result<void> Arrange(const Geometry& geometry) noexcept override;

private:
    bool StartsPaintLayer(usize previous, usize next) const noexcept override;

    struct Track {
        float weight = 0;
        double desired = 0;
        double offset = 0;
    };

    Result<void> PrepareSlot(const GridSlot& slot) noexcept;
    Result<void> SetFill(Array<Track>& tracks, uint32 index, float weight) noexcept;
    static void ArrangeTracks(Array<Track>& tracks, usize count, float available) noexcept;

    Array<GridSlot> slots_;
    mutable Array<Track> columns_;
    mutable Array<Track> rows_;
    mutable usize measuredColumns_ = 0;
    mutable usize measuredRows_ = 0;
};

} // namespace rocks::ui
